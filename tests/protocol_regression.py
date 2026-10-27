"""Offline-only ROS fault orchestration; launches and cleans up only its own C++ children."""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import time


def main():
    parser=argparse.ArgumentParser();parser.add_argument("--workspace",type=Path,required=True);args=parser.parse_args()
    ws=args.workspace.resolve();prefix=ws/"install/anakin_env";sdk=ws/"install/anakin_sdk";out=ws/f"evidence/protocol-regression-{int(time.time()*1000000)}";out.mkdir(exist_ok=True)
    env=os.environ.copy();env.update(ROS_LOCALHOST_ONLY="1",RMW_IMPLEMENTATION="rmw_fastrtps_cpp",AMENT_PREFIX_PATH=f"{sdk}:{prefix}:/opt/ros/humble",LD_LIBRARY_PATH=f"{sdk}/lib:/opt/ros/humble/lib",ANAKIN_WS=str(ws),ANAKIN_PREFIX=str(prefix))
    for case_index,(case,expected_code) in enumerate([("normal",0),("failed_tail",1),("stale_tail",1),("restart",1)]):
        env["ROS_DOMAIN_ID"]=str(96+case_index)
        with (out/f"{case}.fixture.log").open("w") as fixture_log,(out/f"{case}.env.log").open("w") as node_log:
            fixture=subprocess.Popen([str(prefix/"lib/anakin_env/anakin_env_protocol_fixture"),"--ros-args","-p",f"fault:={case}"],env=env,stdout=fixture_log,stderr=subprocess.STDOUT)
            node=None
            try:
                node=subprocess.Popen([str(ws.parent/"anakin_env/en_run.sh"),"-p","duration:=3.5","-p",f"demo_switch:={'true' if case=='normal' else 'false'}"],env=env,stdout=node_log,stderr=subprocess.STDOUT)
                code=node.wait(timeout=8)
                if code!=expected_code:raise RuntimeError(f"{case}: exit {code}, expected {expected_code}")
            finally:
                for process in [node,fixture]:
                    if process is None:continue
                    for sig in [signal.SIGINT,signal.SIGTERM,signal.SIGKILL]:
                        if process.poll() is not None:break
                        process.send_signal(sig)
                        try:process.wait(timeout=1)
                        except subprocess.TimeoutExpired:continue
                    if process.poll() is None:raise RuntimeError("Own child failed bounded cleanup")
        text=(out/f"{case}.env.log").read_text()
        if case=="normal":
            if "completed_modes=3" not in text or "epoch=3" not in text:raise RuntimeError("motion_done did not cause an env-owned new-epoch fallback")
        elif "Local stop latched" not in text or "success=false" not in text:raise RuntimeError(f"{case}: terminal failure was not latched")
        print(f"PASS {case}: exit={code}")


if __name__=="__main__":main()
