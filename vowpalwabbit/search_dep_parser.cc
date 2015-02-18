/*
   Copyright (c) by respective owners including Yahoo!, Microsoft, and
   individual contributors. All rights reserved.  Released under a BSD (revised)
   license as described in the file LICENSE.
   */
#include "search_dep_parser.h"
#include "gd.h"

#define cdep cerr
#undef cdep
#define cdep if (1) {} else cerr
#define val_namespace 100 // valency and distance feature space
#define quad_namespace 101 // namespace for quadratic feature
#define cubic_namespace 102 // namespace for cubic feature
#define offset_const 344429

namespace DepParserTask         {  Search::search_task task = { "dep_parser", run, initialize, finish, NULL, NULL};  }

struct task_data {
  example *ex;
  bool no_quadratic_features;
  bool no_cubic_features;
  bool my_init_flag;
  bool sub_ref;
  bool bad_ref;
  int nfs;
  size_t root_label;
  size_t num_label;
  v_array<uint32_t> valid_actions;
  v_array<uint32_t> valid_labels;
  v_array<uint32_t> gold_action_reward;
  v_array<uint32_t> gold_heads; // gold labels
  v_array<uint32_t> gold_tags; // gold labels
  v_array<uint32_t> *children; // [0]:num_left_arcs, [1]:num_right_arcs; [2]: leftmost_arc, [3]: second_leftmost_arc, [4]:rightmost_arc, [5]: second_rightmost_arc
  v_array<uint32_t> stack; // stack for transition based parser
  v_array<uint32_t> heads; // output array
  v_array<uint32_t> tags; // output array
  v_array<uint32_t> temp;
  v_array<example *> ec_buf;
  vector<string> pairs;
  vector<string> triples;
};

namespace DepParserTask {
  using namespace Search;
  uint32_t max_label = 0;

  void initialize(Search::search& srn, size_t& num_actions, po::variables_map& vm) {
    task_data *data = new task_data();
    data->my_init_flag = false;
    data->ec_buf.resize(12, true);
	data->gold_action_reward.resize(4,true);
    data->children = new v_array<uint32_t>[6]; 

    for(size_t i = 0; i < 6; i++)
      data->children[i] = v_init<uint32_t>();

    data->valid_actions = v_init<uint32_t>();
    data->gold_heads = v_init<uint32_t>();
    data->stack = v_init<uint32_t>();
    data->heads = v_init<uint32_t>();
    data->ec_buf = v_init<example*>();
    data->temp = v_init<uint32_t>();


    srn.set_num_learners(3);
    srn.set_task_data<task_data>(data);
    po::options_description dparser_opts("dependency parser options");
    dparser_opts.add_options()
      ("dparser_no_quad", "Don't use qudaratic features")
      ("dparser_no_cubic","Don't use cubic features")
      ("dparser_bad_ref","Use an abitary bad ref")
      ("dparser_sub_ref","Use an abitary sub-optimal ref")
      ("root_label", po::value<size_t>(&(data->root_label))->default_value(8), "Ensure that there is only one root in each sentence")
      ("num_label", po::value<size_t>(&(data->num_label))->default_value(12), "Number of arc labels");
    srn.add_program_options(vm, dparser_opts);

    data->valid_labels.erase();
    for(size_t i=1; i<=data->num_label;i++)
      if(i!=data->root_label)
        data->valid_labels.push_back(i);

    data->ec_buf.resize(12,true);

    // setup entity and relation labels
    data->no_quadratic_features = (vm.count("dparser_no_quad"))?true:false;
    data->no_cubic_features =(vm.count("dparser_no_cubic"))?true:false;
    data->sub_ref = (vm.count("dparser_sub_ref"))?true:false;
    data->bad_ref =(vm.count("dparser_bad_ref"))?true:false;
    srn.set_options(AUTO_CONDITION_FEATURES);
  }

  void finish(Search::search& srn) {
    task_data *data = srn.get_task_data<task_data>();
    //    dealloc_example(CS::cs_label.delete_label, *(data->ex));
    data->valid_actions.delete_v();
    data->valid_labels.delete_v();
    data->gold_heads.delete_v();
    data->gold_tags.delete_v();
    data->stack.delete_v();
    data->heads.delete_v();
    data->tags.delete_v();
    data->ec_buf.delete_v();
    data->temp.delete_v();
    data->gold_action_reward.delete_v();

    for(size_t i=0; i<6; i++)
      data->children[i].delete_v();
    delete[] data->children;
    delete data;
  } // if we had task data, we'd want to free it here

  void inline add_feature(example *ex,  uint32_t idx, unsigned  char ns, size_t mask, size_t ss){
    feature f = {1.0f, (idx<<ss) & (uint32_t)mask};
    ex->atomics[(int)ns].push_back(f);
  }
  void add_quad_features(Search::search& srn, example *ex){
    size_t ss = srn.get_stride_shift();
    size_t mask = srn.get_mask();
    size_t additional_offset = quad_namespace*offset_const;
    task_data *data = srn.get_task_data<task_data>();
    for(uint32_t idx=0; idx< data->pairs.size(); idx++){
      unsigned char ns_a = data->pairs[idx][0];
      unsigned char ns_b = data->pairs[idx][1];
      for(uint32_t i=0; i< ex->atomics[(int)ns_a].size();i++){
        uint32_t offset = (ex->atomics[(int)ns_a][i].weight_index>>ss) *quadratic_constant+ (uint32_t) additional_offset;
        for(uint32_t j=0; j< ex->atomics[(int)ns_b].size();j++){
          add_feature(ex,  offset+ (ex->atomics[(int)ns_b][j].weight_index>>ss) , quad_namespace, mask, ss);
        }
      }
    }
  }
  void add_cubic_features(Search::search& srn, example *ex){
    size_t ss = srn.get_stride_shift();
    size_t mask = srn.get_mask();
    task_data *data = srn.get_task_data<task_data>();
    size_t additional_offset = cubic_namespace*offset_const;
    for(uint32_t idx=0; idx< data->triples.size(); idx++){
      unsigned char ns_a = data->triples[idx][0];
      unsigned char ns_b = data->triples[idx][1];
      unsigned char ns_c = data->triples[idx][2];
      for(uint32_t i=0; i< ex->atomics[(int)ns_a].size();i++){
        uint32_t offset1 =  (ex->atomics[(int)ns_a][i].weight_index>>ss)*cubic_constant;
        for(uint32_t j=0; j< ex->atomics[(int)ns_b].size();j++){
          uint32_t offset2 =  ((ex->atomics[(int)ns_b][j].weight_index>>ss)+offset1)*cubic_constant2+ (uint32_t)additional_offset;
          for(uint32_t k=0; k< ex->atomics[(int)ns_c].size();k++){
            add_feature(ex, offset2 + ( ex->atomics[(int)ns_c][k].weight_index>>ss) , cubic_namespace, mask, ss);
          }
        }
      }
    }
  }

  void inline reset_ex(example *ex){
    ex->num_features = 0;
    ex->total_sum_feat_sq = 0;
    for(unsigned char *ns = ex->indices.begin; ns!=ex->indices.end; ns++){
      ex->sum_feat_sq[(int)*ns] = 0;
      ex->atomics[(int)*ns].erase();
    }
  }
  // arc-hybrid System.
  uint32_t transition_hybrid(Search::search& srn, uint32_t a_id, uint32_t idx, uint32_t t_id) {
    task_data *data = srn.get_task_data<task_data>();
    v_array<uint32_t> &heads=data->heads, &stack=data->stack, &gold_heads=data->gold_heads, &gold_tags=data->gold_tags, &tags = data->tags;
    v_array<uint32_t> *children = data->children;
    switch(a_id) {
      //SHIFT
      case 1:
        stack.push_back(idx);
        return idx+1;

        //RIGHT
      case 2:
        heads[stack.last()] = stack[stack.size()-2];
        cdep << "make a right link" << stack[stack.size()-2] << " ====> " << (stack.last()) << endl;
        children[5][stack[stack.size()-2]]=children[4][stack[stack.size()-2]];
        children[4][stack[stack.size()-2]]=stack.last();
        children[1][stack[stack.size()-2]]++;
        tags[stack.last()] = t_id;
		srn.loss((gold_heads[stack.last()] != heads[stack.last()])+(gold_tags[stack.last()] != t_id));
/*		if(gold_heads[stack.last()] != heads[stack.last()])
			srn.loss(2);
		else if (gold_tags[stack.last()] != t_id)
			srn.loss(1);
		else
			srn.loss(0);*/
        stack.pop();
        return idx;

        //LEFT
      case 3:
        heads[stack.last()] = idx;
        cdep << "make a left link" << stack.last() << "<==== " << idx << endl;
        children[3][idx]=children[2][idx];
        children[2][idx]=stack.last();
        children[0][idx]++;
        tags[stack.last()] = t_id;
		srn.loss((gold_heads[stack.last()] != heads[stack.last()])+(gold_tags[stack.last()] != t_id));
		/*
		if(gold_heads[stack.last()] != heads[stack.last()])
			srn.loss(2);
		else if (gold_tags[stack.last()] != t_id)
			srn.loss(1);
		else
			srn.loss(0);
			*/
//        srn.loss((gold_heads[stack.last()] != heads[stack.last()]) + (gold_tags[stack.last()] != t_id));
        stack.pop();
        return idx;
    }
    cerr << "Unknown action (search_dep_parser.cc).";
    return idx;
  }

  // This function is only called once
  // We use VW's internal implementation to create second-order and third-order features
  void my_initialize(Search::search& srn, example *base_ex) {
    task_data *data = srn.get_task_data<task_data>();
    vector<string> &newpairs = data->pairs;
    vector<string> &newtriples = data->triples;
    data->ex = alloc_examples(sizeof(base_ex[0].l.multi.label), 1);
    data->nfs = (int) base_ex->indices.size()-1; // remove constant fs
    size_t nfs = data->nfs;

    // setup feature template
    map<string, char> fs_idx_map;
    fs_idx_map["s1"]=0, fs_idx_map["s2"]=1, fs_idx_map["s3"]=2;
    fs_idx_map["b1"]=3, fs_idx_map["b2"]=4, fs_idx_map["b3"]=5;
    fs_idx_map["sl1"]=6, fs_idx_map["sl2"]=7, fs_idx_map["sr1"]=8;
    fs_idx_map["sr2"]=9, fs_idx_map["bl1"]=10, fs_idx_map["bl2"]=11;

    data->ex->indices.push_back(val_namespace);
    for(size_t i=0; i<12*nfs; i++)
      data->ex->indices.push_back((unsigned char)i);

    size_t pos = 0;
    if(!data->no_quadratic_features){
      // quadratic feature encoding
      string quadratic_feature_template = "s1-s2 s1-b1 s1-s1 s2-s2 s3-s3 b1-b1 b2-b2 b3-b3 b1-b2 s1-sl1 s1-sr1 b1-bl1 ENDQ";

      // Generate quadratic features templete
      while ((pos = quadratic_feature_template.find(" ")) != std::string::npos) {
        string token = quadratic_feature_template.substr(0, pos);
        char first_fs_idx = fs_idx_map[token.substr(0,token.find("-"))];
        char second_fs_idx = fs_idx_map[token.substr(token.find("-")+1,token.size())];
        for (size_t i=0; i<nfs; i++) {
          for (size_t j=0; j<nfs; j++) {
            char space_a = (char)(first_fs_idx*nfs+i);
            char space_b = (char)(second_fs_idx*nfs+j);
            newpairs.push_back(string(1, space_a)+ string(1, space_b));
          }
        }
        quadratic_feature_template.erase(0, pos + 1);
      }

      for(size_t i=0; i<6; i++){
        for (size_t j=0; j<nfs; j++) {
          char space_a = (char)(val_namespace);
          char space_b = (char)(i*nfs+j);
          newpairs.push_back(string(1, space_a)+ string(1, space_b));
        }
      }
      char space_a = (char)(val_namespace);
      newpairs.push_back(string(1, space_a)+ string(1, space_a));
      data->ex->indices.push_back(quad_namespace);
    }

    // Generate cubic features

    if(!data->no_cubic_features){
      string cubic_feature_template = "b1-b2-b3 s1-b1-b2 s1-s2-b1 s1-s2-s3 s1-b1-bl1 b1-bl1-bl2 s1-sl1-sl2 s1-s2-s2 s1-sr1-b1 s1-sl1-b1 s1-sr1-sr2 ENDC";
      while ((pos = cubic_feature_template.find(" ")) != std::string::npos) {
        string token = cubic_feature_template.substr(0, pos);
        char first_fs_idx = fs_idx_map[token.substr(0,token.find("-"))];
        token.erase(0, token.find("-")+1);
        char second_fs_idx = fs_idx_map[token.substr(0,token.find("-"))];
        char third_fs_idx = fs_idx_map[token.substr(token.find("-")+1,token.size())];
        for (size_t i=0; i<nfs; i++) {
          for (size_t j=0; j<nfs; j++) {
            for (size_t k=0; k<nfs; k++) {
              char str[3] ={(char)(first_fs_idx*nfs+i), (char)(second_fs_idx*nfs+j), (char)(third_fs_idx*nfs+k)};
              newtriples.push_back(string(1, str[0])+ string(1, str[1])+string(1,str[2]));
            }
          }
        }
        cubic_feature_template.erase(0, pos + 1);
      }
      data->ex->indices.push_back(cubic_namespace);
    }
    data->ex->indices.push_back(constant_namespace);
  }

  // This function needs to be very fast
  void extract_features(Search::search& srn, uint32_t idx,  vector<example*> &ec) {
    task_data *data = srn.get_task_data<task_data>();
    reset_ex(data->ex);
    size_t ss = srn.get_stride_shift();
    size_t mask = srn.get_mask();
    v_array<uint32_t> &stack = data->stack;
    v_array<uint32_t> &tags = data->tags;
    v_array<uint32_t> *children = data->children, &temp=data->temp;
    v_array<example*> &ec_buf = data->ec_buf;
    example &ex = *(data->ex);
    //add constant
    add_feature(&ex, (uint32_t) constant, constant_namespace, mask, ss);
    // be careful: indices in ec starts from 0, but i is starts from 1
    size_t n = ec.size();
    // use this buffer to c_vw()ect the examples, default value: NULL
    for(size_t i=0; i<12; i++)
      ec_buf[i] = 0;

    // feature based on top three examples in stack
    // ec_buf[0]: s1, ec_buf[1]: s2, ec_buf[2]: s3
    for(size_t i=0; i<3; i++)
      ec_buf[i] = (stack.size()>i) ? ec[*(stack.end-(i+1))-1] : 0;

    // features based on examples in string buffer
    // ec_buf[3]: b1, ec_buf[4]: b2, ec_buf[5]: b3
    for(size_t i=3; i<6; i++)
      ec_buf[i] = (idx+(i-3)-1 < n) ? ec[idx+i-3-1] : 0;

    // features based on leftmost and rightmost children of the top element stack
    // ec_buf[6]: sl1, ec_buf[7]: sl2, ec_buf[8]: sr1, ec_buf[9]: sr2;

    for(size_t i=6; i<10; i++)
      if (!stack.empty() && children[i-4][stack.last()]!=0)
        ec_buf[i] = ec[children[i-4][stack.last()]-1];

    // features based on leftmost children of the top element in bufer
    // ec_buf[10]: bl1, ec_buf[11]: bl2
    for(size_t i=10; i<12; i++)
      ec_buf[i] = (idx <=n && children[i-8][idx]!=0)? ec[children[i-8][idx]-1] : 0;


    cdep << "start generating features";

    // unigram features
    size_t nfs = data->nfs;
    uint64_t v0;
    for(size_t i=0; i<12; i++) {
      unsigned char j=0;
      for (unsigned char* fs = ec[0]->indices.begin; fs != ec[0]->indices.end; fs++) {
        if(*fs == constant_namespace) // ignore constant_namespace
          continue;

        uint32_t additional_offset = (uint32_t)((i*nfs+j)*offset_const);
        for(size_t k=0; k<ec[0]->atomics[*fs].size(); k++) {
          if(!ec_buf[i])
            v0 = affix_constant*((j+1)*quadratic_constant + k);
          else
            v0 = (ec_buf[i]->atomics[*fs][k].weight_index>>ss);
          add_feature(&ex, (uint32_t) v0 + additional_offset, (unsigned char)(i*nfs+j), mask, ss);
        }
        j++;
      }
    }
    temp.resize(10,true);
    // distance
    temp[0] = stack.empty()? 0: (idx >n? 1: 2+min(5, idx - stack.last()));

    // #left child of top item in stack
    temp[1] = stack.empty()? 1: 1+min(5, children[0][stack.last()]);

    // #right child of top item in stack
    temp[2] = stack.empty()? 1: 1+min(5, children[1][stack.last()]);

    // #left child of rightmost item in buf
    temp[3] = idx>n? 1: 1+min(5 , children[0][idx]);
	/*
    for(size_t i=4; i<8; i++)
      if (!stack.empty() && children[i-2][stack.last()]!=0)
        temp[i] = tags[children[i-2][stack.last()]];
	  else
  	    temp[i] = 15;

    // features based on leftmost children of the top element in bufer
    // ec_buf[10]: bl1, ec_buf[11]: bl2
    for(size_t i=8; i<10; i++)
        temp[i] = (idx <=n && children[i-6][idx]!=0)? tags[children[i-6][idx]] : 15;
	*/

    size_t additional_offset = val_namespace*offset_const; 
    for(int j=0; j< 4;j++) {
      add_feature(&ex, temp[j]+ additional_offset , val_namespace, mask, ss);
	}
    // action history
    if(!data->no_quadratic_features)
      add_quad_features(srn, data->ex);
    if(!data->no_cubic_features)
      add_cubic_features(srn, data->ex);
    size_t count=0;
    for (unsigned char* ns = data->ex->indices.begin; ns != data->ex->indices.end; ns++) {
      data->ex->sum_feat_sq[(int)*ns] = (float) data->ex->atomics[(int)*ns].size();
      count+= data->ex->atomics[(int)*ns].size();
    }
    data->ex->num_features = count;
    data->ex->total_sum_feat_sq = (float) count;
  }

  void get_valid_actions(v_array<uint32_t> & valid_action, uint32_t idx, uint32_t n, uint32_t stack_depth) {
    valid_action.erase();
    // SHIFT
    if(idx<=n)
      valid_action.push_back(1);

    // RIGHT
    if(stack_depth >=2)
      valid_action.push_back(2);

    // LEFT
    if(stack_depth >=1 && idx<=n)
      valid_action.push_back(3);
  }

  bool is_valid(uint32_t action, v_array<uint32_t> valid_actions) {
    for(size_t i=0; i< valid_actions.size(); i++)
      if(valid_actions[i] == action)
        return true;
    return false;
  }

  size_t get_sub_gold_actions(Search::search &srn, uint32_t idx, uint32_t n){
    task_data *data = srn.get_task_data<task_data>();
    v_array<uint32_t> &gold_action_reward = data->gold_action_reward, &stack = data->stack, &gold_heads=data->gold_heads, &valid_actions=data->valid_actions;

    // gold = SHIFT
    if (is_valid(1,valid_actions) &&( stack.empty() || gold_heads[idx] == stack.last()))
		return 1;

    // gold = LEFT
    if (is_valid(3,valid_actions) && gold_heads[stack.last()] == idx)
		return 3;
	return 0;
  }

  size_t get_gold_actions(Search::search &srn, uint32_t idx, uint32_t n){
    task_data *data = srn.get_task_data<task_data>();
    v_array<uint32_t> &gold_action_reward = data->gold_action_reward, &stack = data->stack, &gold_heads=data->gold_heads, &valid_actions=data->valid_actions;

    // gold = SHIFT
    if (is_valid(1,valid_actions) &&( stack.empty() || gold_heads[idx] == stack.last()))
		return 1;

    // gold = LEFT
    if (is_valid(3,valid_actions) && gold_heads[stack.last()] == idx)
		return 3;

	for(size_t i = 1; i<= 3; i++)
		gold_action_reward[i] = 500;

    // dependency with SHIFT
    for(uint32_t i = 0; i<stack.size(); i++)
   	  if(idx <=n && (gold_heads[stack[i]] == idx || gold_heads[idx] == stack[i]))
		  gold_action_reward[1] -= 1;

    // dependency with left and right
    for(uint32_t i = idx+1; i<=n; i++)
   	  if(gold_heads[i] == stack.last()|| gold_heads[stack.last()] == i) {
          gold_action_reward[2] -=1;
		  gold_action_reward[3] -=1;
	  }

	// break the tie between left and right
	if(stack.size()>=2 && gold_heads[stack.last()] == stack[stack.size()-2])
		gold_action_reward[3] -= 10;

	// remove invalid actions
	for(size_t i=1; i<=3; i++)
      if (!is_valid(i,valid_actions))
		  gold_action_reward[i] = 0;

	// return the best action
	size_t best_action = 1;
	for(size_t i=1; i<=3; i++)
		if(gold_action_reward[i] >= gold_action_reward[best_action])
			best_action= i;
	return best_action;
  }

  void run(Search::search& srn, vector<example*>& ec) {
    cdep << "start structured predict"<<endl;
    task_data *data = srn.get_task_data<task_data>();

    v_array<uint32_t> &stack=data->stack, &gold_heads=data->gold_heads, &valid_actions=data->valid_actions, &heads=data->heads, &gold_tags=data->gold_tags, &tags=data->tags, &valid_labels=data->valid_labels;
    uint32_t n = (uint32_t) ec.size();

    uint32_t idx = 2;

    // initialization
    if(!data->my_init_flag) {
      my_initialize(srn, ec[0]);
      data->my_init_flag = true;
    }    
    heads.resize(ec.size()+1, true);
    tags.resize(ec.size()+1, true);
    gold_heads.erase();
    gold_heads.push_back(0);
    gold_tags.erase();
    gold_tags.push_back(0);
    for(size_t i=0; i<ec.size(); i++) {
      uint32_t label = ec[i]->l.multi.label;
      gold_heads.push_back((label & 255) -1);
      gold_tags.push_back(label >>8);
      heads[i+1] = 0;
      tags[i+1] = -1;
    }
    stack.erase();
    stack.push_back(1);
    for(size_t i=0; i<6; i++){
      data->children[i].resize(ec.size()+1, true);
      for(size_t j=0; j<ec.size()+1; j++)
        data->children[i][j] = 0;
    }

    int count=0;
    cdep << "start decoding"<<endl;
    while(stack.size()>1 || idx <= n){
      if(srn.predictNeedsExample())
        extract_features(srn, idx, ec);
      get_valid_actions(valid_actions, idx, n, (uint32_t) stack.size());
      uint32_t gold_action = get_gold_actions(srn, idx, n);
      if(data->bad_ref)
		  gold_action = valid_actions[0];
	  if(data->sub_ref){
          gold_action = get_sub_gold_actions(srn, idx, n);
		  if(gold_action==0) gold_action = valid_actions[0];
	  }
      // Predict the next action {SHIFT, REDUCE_LEFT, REDUCE_RIGHT}
      uint32_t a_id= Search::predictor(srn, (ptag) 0).set_input(*(data->ex)).set_oracle(gold_action).set_allowed(valid_actions).set_condition_range(count, srn.get_history_length(), 'p').set_learner_id(0).predict();
      count++;
      uint32_t t_id = 0;
      if(a_id ==2 || a_id == 3){
	  	uint32_t gold_label = gold_tags[stack.last()];
		if(data->bad_ref)
			gold_label = 0;
		if(data->sub_ref)
			gold_label = gold_tags[stack.last()];
        t_id= Search::predictor(srn, (ptag) count+1).set_input(*(data->ex)).set_oracle(gold_label).set_allowed(valid_labels).set_condition_range(count, srn.get_history_length(), 'p').set_learner_id(a_id-1).predict();
        count++;
      }
      idx = transition_hybrid(srn, a_id, idx, t_id);
    }
    heads[stack.last()] = 0;
    tags[stack.last()] = data->root_label;
    cdep << "root link to the last element in stack" <<  "root ====> " << (stack.last()) << endl;
    srn.loss((gold_heads[stack.last()] != heads[stack.last()]));
    if (srn.output().good())
      for(size_t i=1; i<=n; i++) {
        cdep << heads[i] << " ";
        srn.output() << (heads[i])<<":"<<tags[i] << endl;
      }
    cdep << "end structured predict"<<endl;
  }
}
