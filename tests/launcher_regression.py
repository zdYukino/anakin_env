"""Offline fault: stop only the launcher's own MC child and require bounded cleanup."""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import time


def main():
    parser=argparse.ArgumentParser();parser.add_argument("--workspace",type=Path,required=True);args=parser.parse_args()
    ws=args.workspace.resolve();log=ws/f"evidence/launcher-cleanup-{int(time.time()*1000000)}.log"
    env=os.environ.copy();env.update(ANAKIN_WS=str(ws),ROS_DOMAIN_ID="95")
    children=[];launcher=None
    with log.open("w") as out:
        try:
            launcher=subprocess.Popen([str(ws.parent/"anakin_env/scripts/integration_run.sh"),"--offline-test"],env=env,stdout=out,stderr=subprocess.STDOUT)
            mc=None;deadline=time.monotonic()+5
            while time.monotonic()<deadline and launcher.poll() is None:
                child_file=Path(f"/proc/{launcher.pid}/task/{launcher.pid}/children")
                if child_file.exists():
                    children=[int(v) for v in child_file.read_text().split()]
                    for pid in children:
                        try:exe=os.readlink(f"/proc/{pid}/exe")
                        except FileNotFoundError:continue
                        if exe.endswith("/anakin_mc_node"):mc=pid
                if mc is not None:break
                time.sleep(.02)
            if mc is None:raise RuntimeError("Own MC child was not found")
            os.kill(mc,signal.SIGSTOP)
            started=time.monotonic();launcher.send_signal(signal.SIGTERM);code=launcher.wait(timeout=5);elapsed=time.monotonic()-started
            if code!=143 or elapsed>=4.5:raise RuntimeError(f"Unbounded/incorrect launcher exit: code={code} elapsed={elapsed}")
            survivors=[pid for pid in children if Path(f"/proc/{pid}").exists()]
            if survivors:raise RuntimeError(f"Owned children survived launcher cleanup: {survivors}")
            print(f"PASS stopped own MC: launcher_exit={code}, cleanup_seconds={elapsed:.3f}, survivors=0, log={log}")
        finally:
            if launcher is not None and launcher.poll() is None:
                launcher.kill();launcher.wait(timeout=1)
                for pid in children:
                    try:os.kill(pid,signal.SIGKILL)
                    except ProcessLookupError:pass


if __name__=="__main__":main()
