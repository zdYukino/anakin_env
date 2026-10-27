#include <mujoco/mujoco.h>
#include <iomanip>
#include <iostream>
int main(int argc,char** argv) {
 if(argc!=2)return 2;char error[1024]{};
 mjModel* m=mj_loadXML(argv[1],nullptr,error,sizeof(error));
 if(!m){std::cerr<<error<<'\n';return 1;}
 std::cout<<std::setprecision(17)<<"{\"schema\":\"g1_sim_mjcf_budget_audit_v1\",\"mujoco_version\":\""<<mj_versionString()<<"\",\"timestep_s\":"<<m->opt.timestep<<",\"nu\":"<<m->nu<<",\"nq\":"<<m->nq<<",\"actuators\":[";
 for(int a=0;a<m->nu;++a) {
  const int j=m->actuator_trnid[2*a];if(j<0||j>=m->njnt)return 3;
  if(a)std::cout<<',';
  std::cout<<"{\"index\":"<<a<<",\"joint\":\""<<mj_id2name(m,mjOBJ_JOINT,j)<<"\",\"transmission_type\":"<<m->actuator_trntype[a]<<",\"gear\":[";
  for(int k=0;k<6;++k){if(k)std::cout<<',';std::cout<<m->actuator_gear[6*a+k];}
  std::cout<<"],\"joint_type\":"<<m->jnt_type[j]<<",\"position_limited\":"<<int(m->jnt_limited[j])<<",\"position_range\":["<<m->jnt_range[2*j]<<','<<m->jnt_range[2*j+1]<<"],\"joint_margin\":"<<m->jnt_margin[j]<<",\"joint_solref\":["<<m->jnt_solref[mjNREF*j]<<','<<m->jnt_solref[mjNREF*j+1]<<"],\"control_limited\":"<<int(m->actuator_ctrllimited[a])<<",\"control_range\":["<<m->actuator_ctrlrange[2*a]<<','<<m->actuator_ctrlrange[2*a+1]<<"],\"actuator_force_limited\":"<<int(m->actuator_forcelimited[a])<<",\"actuator_force_range\":["<<m->actuator_forcerange[2*a]<<','<<m->actuator_forcerange[2*a+1]<<"],\"joint_actuator_force_limited\":"<<int(m->jnt_actfrclimited[j])<<",\"joint_actuator_force_range\":["<<m->jnt_actfrcrange[2*j]<<','<<m->jnt_actfrcrange[2*j+1]<<"],\"gain_type\":"<<m->actuator_gaintype[a]<<",\"bias_type\":"<<m->actuator_biastype[a]<<",\"dynamics_type\":"<<m->actuator_dyntype[a]<<",\"gain0\":"<<m->actuator_gainprm[mjNGAIN*a]<<",\"velocity_limit_available\":false}";
 }
 std::cout<<"],\"missing_execution_budgets\":[\"per_joint_velocity_limit\",\"near_limit_guard_or_validated_stopping_envelope\",\"maximum_saturation_duration\"]}\n";
 mj_deleteModel(m);
}
