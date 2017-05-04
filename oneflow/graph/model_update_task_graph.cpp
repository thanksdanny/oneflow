#include "graph/model_update_task_graph.h"
#include "operator/operator_manager.h"

namespace oneflow {

MdUpdtTaskGraph::MdUpdtTaskGraph(
    const ChainNode* data_chain,
    const std::vector<CompTaskNode*>& sorted_bp_comptasks4data_chain) {
  BuildTaskGraph(data_chain);
  HashMap<uint64_t, CompTaskNode*> parallel_id2updt;
  InitFaker2MccoyAndParallelId2UpdtMap(sorted_bp_comptasks4data_chain,
                                       &parallel_id2updt);
  BuildExecAndEnrollLbn2Regsts();
  CompleteUpdateTask(sorted_bp_comptasks4data_chain, parallel_id2updt);
}

void MdUpdtTaskGraph::BuildTaskGraph(const ChainNode* data_chain) {
  // Construct ModelUpdateOp
  OperatorConf op_conf;
  op_conf.set_name("model_update_" + data_chain->ConcatedOpsName());
  op_conf.mutable_model_update_conf();
  auto model_update_op = OpMgr::Singleton().ConstructOp(op_conf);
  // ModelUpdateChain
  auto chain_gph = of_make_unique<ChainGraph> ();
  ChainNode* updt_chain = chain_gph->NewNode();
  updt_chain->mut_op_vec() = {model_update_op};
  auto parallel_desc4updt = new ParallelDesc(*(data_chain->parallel_desc()));
  parallel_desc4updt->mut_policy() = kModelParallel;
  updt_chain->mut_parallel_desc().reset(parallel_desc4updt);
  // FakerChain
  if (data_chain->parallel_desc()->policy() == kDataParallel) {
    ChainNode* faker_chain = chain_gph->NewNode();
    faker_chain->mut_op_vec().clear();
    faker_chain->mut_parallel_desc() = data_chain->parallel_desc();
    faker_chain->mut_output_lbns() = {RegstDesc::kAllLbn};
    updt_chain->mut_input_lbns() = {RegstDesc::kAllLbn};
    Connect(faker_chain, chain_gph->NewEdge(), updt_chain);
  }
  //
  chain_gph->UpdateSourceAndSink();
  std::string dot_filepath_prefix =
      LogDir() + "/model_update_" + data_chain->node_id_str() + "_";
  chain_gph->ToDotFile(dot_filepath_prefix + "chain_graph.dot");
  BuildFromChainGph(std::move(chain_gph), false, dot_filepath_prefix);
}

void MdUpdtTaskGraph::InitFaker2MccoyAndParallelId2UpdtMap(
    const std::vector<CompTaskNode*>& sorted_bp_comptasks4data_chain,
    HashMap<uint64_t, CompTaskNode*>* parallel_id2updt) {
  std::vector<CompTaskNode*> comptasks4faker_chain;
  for (const std::unique_ptr<TaskNode>& node : nodes()) {
    CompTaskNode* comp_node = dynamic_cast<CompTaskNode*> (node.get());
    if (!comp_node) { continue; }
    if (comp_node->IsFaker()) {
      comptasks4faker_chain.push_back(comp_node);
    } else {
      parallel_id2updt->emplace(comp_node->parallel_id(), comp_node);
    }
  }
  SortByParallelId(&comptasks4faker_chain);
  if (chain_gph()->nodes().size() == 2) {
    CHECK_EQ(comptasks4faker_chain.size(),
             sorted_bp_comptasks4data_chain.size());
  }
  for (size_t i = 0; i < comptasks4faker_chain.size(); ++i) {
    EnrollFakerMccoy(comptasks4faker_chain[i],
                     sorted_bp_comptasks4data_chain[i]);
  }
}

void MdUpdtTaskGraph::CompleteUpdateTask(
    const std::vector<CompTaskNode*>& sorted_bp_comptasks4data_chain,
    const HashMap<uint64_t, CompTaskNode*>& parallel_id2updt) {
  for (CompTaskNode* bp_task : sorted_bp_comptasks4data_chain) {
    // useful vars
    uint64_t parallel_id = bp_task->parallel_id();
    CompTaskNode* update_task = parallel_id2updt.at(parallel_id);
    TaskNode* fw_task = bp_task->GetFwNode();
    update_task->TakeOverRegstDesc(fw_task, "model");
    update_task->TakeOverRegstDesc(fw_task, "model_tmp");
    RegstDesc* model_diff_regst = bp_task->GetProducedRegstDesc("model_diff");
    RegstDesc* model_regst = update_task->GetProducedRegstDesc("model");
    // complete update task
    ExecNode* update_exec = update_task->exec_gph().SoleNode();
    const std::string ibn = "model_diffs";
    if (update_task->in_edges().empty()) {
      update_exec->BindBnInOpAndRegst(ibn, model_diff_regst);
    } else {
      update_exec->BindBnInOpAndRegst(
          ibn, GetRelatedRegst(update_task->SoleInEdge()));
    }
    update_exec->BindBnInOpAndRegst(update_exec->op()->SoleObn(), model_regst);
  }
}

} // namespace oneflow
