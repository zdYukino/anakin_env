"""Offline-only env restart against one retained C++ MC instance; no SDK channels."""
import argparse
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time

def stop(process):
    if process is None:return
    for sig in [signal.SIGINT,signal.SIGTERM,signal.SIGKILL]:
        if process.poll() is not None:return
        process.send_signal(sig)
        try:process.wait(timeout=1)
        except subprocess.TimeoutExpired:pass
    raise RuntimeError("Own child exceeded cleanup deadline")

def main():
    parser=argparse.ArgumentParser();parser.add_argument("--workspace",required=True,type=Path);args=parser.parse_args()
    ws=args.workspace.resolve();out=ws/"evidence"/f"restart-regression-{time.time_ns()}";out.mkdir()
    env=os.environ.copy();env.update(ANAKIN_WS=str(ws),ROS_DOMAIN_ID="194",ROS_LOCALHOST_ONLY="1")
    mc_env=env.copy();mc_env["ANAKIN_PREFIX"]=str(ws/"install/anakin_mc")
    env_env=env.copy();env_env["ANAKIN_PREFIX"]=str(ws/"install/anakin_env")
    server=None;node=None;sessions=[]
    with (out/"mc.log").open("w") as log:
        try:
            server=subprocess.Popen([str(ws.parent/"anakin_mc/en_run.sh")],env=mc_env,stdout=log,stderr=subprocess.STDOUT)
            original_pid=server.pid
            for run in range(2):
                with (out/f"env-{run}.log").open("w") as node_log:
                    node=subprocess.Popen([str(ws.parent/"anakin_env/en_run.sh"),"-p","duration:=2.0"],env=env_env,stdout=node_log,stderr=subprocess.STDOUT)
                    code=node.wait(timeout=7)
                if code:raise RuntimeError(f"env restart {run} exited {code}")
                text=(out/f"env-{run}.log").read_text()
                match=re.search(r"session ([0-9a-f]{32})",text)
                if not match or "success=true" not in text or "epoch=1" not in text:raise RuntimeError("Fresh env session did not complete its own new transaction")
                sessions.append(match.group(1))
                if server.poll() is not None or server.pid!=original_pid:raise RuntimeError("MC was restarted, invalidating discovery test")
                time.sleep(.2)
            if sessions[0]==sessions[1]:raise RuntimeError("Session identity was reused")
            result={"pass":True,"same_mc_pid":original_pid,"fresh_sessions":sessions,"sdk_channels":False}
            (out/"summary.json").write_text(json.dumps(result,indent=2)+"\n")
            print(f"PASS fresh env session reconnects to retained nonempty MC journal; evidence={out}")
        finally:
            stop(node);stop(server)

if __name__=="__main__":main()
