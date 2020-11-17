#include <cstring>
#include <iostream>
#include <fstream>
#include <iterator>
#include <sys/stat.h>
#include <sys/types.h>
#include <assert.h>
#include <ert.h>
#include <unistd.h>
#include <xrt.h>
#include <algorithm>
#include <chrono>
#include <regex>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <iomanip>
#include <math.h>
#include "engine.hpp"
#include "dpuv4e_controller.hpp"
#include "xir/xir.h"
#include "xir/graph/graph.hpp"
#include "xir/graph/subgraph.hpp"
#include "json-c/json.h"

#include "dpu_runner.hpp"
#include "xir/tensor/tensor.hpp"
#include "vart/tensor_buffer.hpp"


#include "vitis/ai/env_config.hpp"
#include "vitis/ai/profiling.hpp"
#include "device_handle.hpp"
using namespace std;
using namespace chrono;
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#define BIT(nr) (1 << (nr))

#define XDPU_GLOBAL_INT_ENABLE BIT(0)
#define XDPU_CONTROL_AP 0x00
#define XDPU_CONTROL_AP_START 0x00
#define XDPU_CONTROL_GIE 0x04
#define XDPU_CONTROL_IER 0x08
#define XDPU_CONTROL_ISR 0x0C
#define BASE_ADDR 0x00000 // NOTE: USE ONLY FOR BEFORE 2020.1
#define XDPU_CONTROL_START 0x10  /* write 1 to enable DPU start */
#define XDPU_CONTROL_RESET 0x1C  /* reset DPU active low */
#define XDPU_CONTROL_HP 0x20
#define XDPU_CONTROL_INSTR_L 0x140
#define XDPU_CONTROL_INSTR_H 0x144
#define XDPU_CONTROL_ADDR_0_L 0x100 // weights / reg0
#define XDPU_CONTROL_ADDR_0_H 0x104
#define XDPU_CONTROL_ADDR_1_L 0x108 // input1
#define XDPU_CONTROL_ADDR_1_H 0x10C

#define BATCHSIZE 8

DEF_ENV_PARAM(DPU_IP_LATENCY, "0");
DEF_ENV_PARAM(XLNX_ENABLE_DUMP, "0");
DEF_ENV_PARAM(XLNX_ENABLE_DEBUG_MODE, "0");
DEF_ENV_PARAM(XLNX_ENABLE_FINGERPRINT_CHECK, "1");
/*
 * a contiguous memory block is allocated for each requests' I/O
 * layout:
 *  0      : output
 *  3071   : input
 *  153599 : intermediate
 */
//#define XDPU_IO_INPUT_OFFSET      3071
//#define XDPU_IO_OUTPUT_OFFSET        0
//#define XDPU_IO_TOTAL_SIZE    11018751
//static size_t XDPU_IO_INPUT_OFFSET;
//static size_t XDPU_IO_OUTPUT_OFFSET;
//static size_t XDPU_IO_TOTAL_SIZE;
#define DPUREG_MISC_END 0x84
#define DPUREG_CONV_END 0x88
#define DPUREG_SAVE_END 0x8c
#define DPUREG_LOAD_END 0x90
#define DPUREG_MISC_START 0x94
#define DPUREG_CONV_START 0x98
#define DPUREG_SAVE_START 0x9c
#define DPUREG_LOAD_START 0xa0
#define DPUREG_CYCLE_COUNTER 0xa8
#define VERSION_CODE_L 0x1f0
#define VERSION_CODE_H 0x1f4

static uint32_t read32_dpu_reg(xclDeviceHandle dpu_handle, uint64_t offset) {
  uint32_t val;
  xclRead(dpu_handle, XCL_ADDR_KERNEL_CTRL, offset, (void *)(&val), 4);
  return val;
}

DpuV4eController::DpuV4eController(std::string meta) 
  : XclDpuController<XrtDeviceHandle, XrtDeviceBuffer, XrtDeviceBuffer>(meta),dump_mode_(false),debug_mode_(false) {
  // assign many contexts -- one for each worker thread
  // threads cannot share contexts (or xclExecWait may miss the 'done' signal)
  Engine& engine = Engine::get_instance();
  for (unsigned i=0; i < engine.get_num_workers(); i++)
    contexts_.emplace_back(new XrtContext(*handle_));

  init(meta);
}

DpuV4eController::DpuV4eController(const xir::Subgraph *subgraph) 
  : XclDpuController<XrtDeviceHandle, XrtDeviceBuffer, XrtDeviceBuffer>(subgraph),dump_mode_(false),debug_mode_(false) {
  Engine& engine = Engine::get_instance();
  for (unsigned i=0; i < engine.get_num_workers(); i++)
    contexts_.emplace_back(new XrtContext(*handle_));

  init(subgraph);
}
DpuV4eController::~DpuV4eController() {
}
std::vector<vart::TensorBuffer*> DpuV4eController::init_tensor_buffer(std::vector<const xir::Tensor*> tensors) {
 std::vector<vart::TensorBuffer*>  tbufs;
 for (unsigned bs=0; bs < BATCHSIZE; bs++) {
    for (unsigned ti=0; ti < tensors.size(); ti++)
    {
      // allocate aligned host memory
      const size_t dataSize = 1;//vart::size_of(tensors[ti]->get_data_type());
      size_t size = tensors[ti]->get_element_num() * dataSize;
      void *data;
      if (posix_memalign(&data, getpagesize(), size))
        throw std::bad_alloc();

      // make TensorBuffer to hold host memory
      std::unique_ptr<vart::CpuFlatTensorBuffer> tbuf(
        new vart::CpuFlatTensorBuffer(data, tensors[ti]));
      tbufs.emplace_back(tbuf.get());
      {
        std::unique_lock<std::mutex> lock(hwbuf_mtx_);
        bufs_.emplace_back(std::move(tbuf));
        //if(isInput)
        //  input_tensor_buffers_.emplace_back(std::move(tbuf));
        //else
        //  output_tensor_buffers_.emplace_back(std::move(tbuf));
      }
    }
  }
  return tbufs;

}

void DpuV4eController::init(const xir::Subgraph *subgraph) {
  if (subgraph->has_attr("device")&&(subgraph->get_attr<std::string>("device")=="DPU")) {
    init_graph(subgraph);
  }
  else
    throw std::runtime_error("Error: subgraph is not supported in DPURunner");

}
void DpuV4eController::init(const std::string &meta) {

  // get directory of meta.json
  size_t slash = meta.find_last_of("/");
  auto dirpath = meta.substr(0, slash);
  std::ifstream f(meta);
  std::stringstream metabuf;
  metabuf << f.rdbuf();
  json_object *jobj = json_tokener_parse(metabuf.str().c_str());     

  // get xmode name
  json_object *modelObj = NULL;
  if (!json_object_object_get_ex(jobj, "filename", &modelObj))
    throw std::runtime_error("Error: missing 'filename' field in meta.json");

  std::string modelName = json_object_get_string(modelObj);
  cout << modelName<< endl;
  const string xmodel = dirpath + "/" +modelName;
 
  if (json_object_object_get_ex(jobj, "dump_mode", &modelObj)) {
    dump_mode_ = json_object_get_boolean(modelObj);
  }  
  if (json_object_object_get_ex(jobj, "debug_mode", &modelObj)) {
    debug_mode_ = json_object_get_boolean(modelObj);
  }  

  // Get all subgraphs
  auto graph = xir::Graph::deserialize(xmodel);
  auto root = graph->get_root_subgraph();
  auto children = root->children_topological_sort();
  std::vector<xir::Subgraph*> subgraph;
  for (auto c : children) {
    auto device = c->get_attr<std::string>("device");
    if (device == "DPU") {
      subgraph.emplace_back(c);
    }
  }
  init_graph(subgraph[0]);
}
static const xir::Tensor* find_tensor(const xir::Tensor* in_tensor, const xir::Subgraph* subgraph,bool isInput) {
    auto op_tmp = in_tensor->get_producer();
    auto out = op_tmp->get_output_tensor();
    if ((!isInput)&&(op_tmp->get_type() == "download")) {
      auto input_ops = op_tmp->get_input_ops("input");
      out = input_ops[0]->get_output_tensor();
    } else if (!out->has_attr("reg_id")) {
      auto fanout_ops = op_tmp->get_fanout_ops();
      auto subgraph_ops = subgraph->get_ops();
      auto ops = std::vector<const xir::Op*>();
      std::set_intersection(fanout_ops.begin(), fanout_ops.end(),
                            subgraph_ops.begin(), subgraph_ops.end(),
                            std::back_inserter(ops));
      auto upload_op = ops.front();
      out = upload_op->get_output_tensor();

  }
  return out;

}
void DpuV4eController::init_graph(const xir::Subgraph* subgraph) {
  auto handle = contexts_[0]->get_dev_handle();
  if(ENV_PARAM(XLNX_ENABLE_FINGERPRINT_CHECK)) {
    if (subgraph->has_attr("dpu_fingerprint")) {
      const uint64_t fingerprint = subgraph->get_attr<std::uint64_t>("dpu_fingerprint");
      uint32_t low = read32_dpu_reg(handle,  0+ VERSION_CODE_L);
      uint32_t high = read32_dpu_reg(handle,  0+ VERSION_CODE_H);
      uint64_t version = high;
      version = (version << 32) + low;
      if (version != fingerprint)
        throw std::runtime_error("Error: subgraph's version is mismatch with xclbin");
    } else {

      throw std::runtime_error("Error: no hardware info in subgraph");

    }
  }
  xclBOProperties boProp;
  dump_mode_ = dump_mode_|| ENV_PARAM(XLNX_ENABLE_DUMP);
  debug_mode_ = debug_mode_|| ENV_PARAM(XLNX_ENABLE_DEBUG_MODE);
  if(dump_mode_) {
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()); 
    std::stringstream ss;
    ss << "dump_" << std::put_time(std::localtime(&t), "%Y%m%d%H%M%S"); 
    dump_folder_ = ss.str();
    if(mkdir(dump_folder_.c_str(),S_IRUSR | S_IWUSR | S_IXUSR | S_IRWXG | S_IRWXO))
      throw std::runtime_error("Error: Create dump folder error");
    for(auto i = 0;i<BATCHSIZE;i++) {
      std::string tmp = dump_folder_ + "/E" + std::to_string(i);
      if(mkdir(tmp.c_str(),S_IRUSR | S_IWUSR | S_IXUSR | S_IRWXG | S_IRWXO))
        throw std::runtime_error("Error: Create dump sub folder error"); 
    } 
  }


  //auto graph = subgraph->get_graph();
  // Get one subgraph
  const xir::Subgraph* subgraph_ = subgraph; // check subgraph_->get_name()

  // Load parameter
  size_t parameter_size = 0;
  const char * parameter_value = NULL;
  std::map<std::string, std::vector<char>> reg_id_to_parameter_value;
  if (subgraph_->has_attr("reg_id_to_parameter_value")) {
    reg_id_to_parameter_value =
      subgraph_->get_attr<std::map<std::string, std::vector<char>>>("reg_id_to_parameter_value");
    for (const auto& c : reg_id_to_parameter_value) {
      if (!c.second.empty()) {
        parameter_size = c.second.size();
        parameter_value = (const char *)&c.second[0];
        break;
      }
    }
  }
  // Get Reg ID size
  auto reg_id_to_context_type =
    subgraph_->get_attr<std::map<std::string, std::string>>("reg_id_to_context_type");
  auto reg_id_to_size =
    subgraph_->get_attr<std::map<std::string, int32_t>>("reg_id_to_size");
  size_t total_io_size = 0;
  for(auto &r : reg_id_to_context_type) {
    if (r.second == "DATA") {
      total_io_size += reg_id_to_size.at(r.first);
    }
  }
  xdpu_io_total_size = total_io_size;

  layer_info layer(subgraph_->get_name()); 
  // Get input offset
  auto input_tensors = subgraph_->get_input_tensors();
  auto output_tensors = subgraph_->get_output_tensors();
  for (auto &in_tensor : input_tensors) {
    auto out = find_tensor(in_tensor,subgraph_,true);
    auto ddr_addr = out->get_attr<std::int32_t>("ddr_addr");
    xdpu_io_input_offset.emplace_back(ddr_addr);
    input_scales_.push_back(pow(2,in_tensor->get_attr<std::int32_t>("fix_point")));
    input_dims.emplace_back(in_tensor->get_shape());
    layer.inputs.emplace_back(address_info(ddr_addr, 
      in_tensor->get_data_size(), layer_info::name_map(out->get_name()))); 
    auto attrs = out->get_attrs(); 
    xir::Tensor *tensor = xir::Tensor::create(in_tensor->get_name(), in_tensor->get_shape(), in_tensor->get_data_type()).release();
    tensor->set_attrs(std::move(attrs));
    input_tensors_.emplace_back(tensor);

  }

  // Get output offset
  for(auto &out_tensor : output_tensors) {
    auto out = find_tensor(out_tensor,subgraph_,false);
    auto ddr_addr = out->get_attr<std::int32_t>("ddr_addr");
    xdpu_io_output_offset.emplace_back(ddr_addr);
    output_scales_.push_back(pow(2,(-1)*out_tensor->get_attr<std::int32_t>("fix_point")));
    output_dims.emplace_back(out->get_shape()); 
    layer.outputs.emplace_back(std::make_tuple(ddr_addr, 
        out_tensor->get_data_size(), layer_info::name_map(out->get_name())));
    auto attrs = out->get_attrs();
    //std::unique_ptr<xir::Tensor> tensor(
    //  new xir::Tensor(out_name, out->get_shape(),xir::Tensor::DataType::INT8));
    xir::Tensor *tensor = xir::Tensor::create(out_tensor->get_name(), out_tensor->get_shape(), out_tensor->get_data_type()).release();
    tensor->set_attrs(std::move(attrs));
    //auto tensor = out;
    output_tensors_.emplace_back(tensor);

  }
  
  //in release mode: using dbg_layers_ to store first inputs and final outputs information  
  dbg_layers_.clear();
  dbg_layers_.emplace_back(std::move(layer));

  // Load mc_code
  if(!debug_mode_) { 
    auto& mc_code = subgraph_->get_attr<std::vector<char>>("mc_code");
    void *codePtr = NULL;
    unsigned size = mc_code.size();
    if (posix_memalign(&codePtr, getpagesize(), size))
      throw std::bad_alloc();
    for (unsigned i=0; i < size; i++) ((char*)codePtr)[i] = mc_code[i];
    auto codeMem 
      = xclAllocUserPtrBO(handle, codePtr, size, handle_->get_device_info().ddr_bank);
    xclSyncBO(handle, codeMem, XCL_BO_SYNC_BO_TO_DEVICE, size, 0);
    xclGetBOProperties(handle, codeMem, &boProp);
    code_addr_ = boProp.paddr;
    free(codePtr);

  } else {
    auto children = subgraph_->get_children(); 
    auto child_order = subgraph_->get_attr<std::vector<std::string>>("children_topological_sort");
    for (const auto& child_name : child_order) { 
      auto child = std::find_if(children.begin(), children.end(),
          [&child_name](auto g) { return g->get_name() == child_name; });

      layer_info layer(child_name); 
      if ((*child)->has_attr("mc_code")) {
        auto& mc_code = (*child)->get_attr<std::vector<char> >("mc_code"); 

        void *codePtr = NULL;
        auto codeSize = mc_code.size(); 
        if (posix_memalign(&codePtr, getpagesize(), codeSize))
          throw std::bad_alloc(); 
        std::copy(mc_code.begin(), mc_code.end(),  (char*)codePtr); 
        auto codeBO = xclAllocUserPtrBO(handle, codePtr, codeSize, handle_->get_device_info().ddr_bank); 
        xclSyncBO(handle, codeBO, XCL_BO_SYNC_BO_TO_DEVICE, codeSize, 0);
        
        std::get<0>(layer.code_addr) = (uint64_t)xclGetDeviceAddr(handle, codeBO);
        std::get<1>(layer.code_addr) = codeSize;
        
        free(codePtr); 
      }    
 
      auto in_tensors = (*child)->get_input_tensors() ;
      for(auto t: in_tensors) {
        layer.inputs.emplace_back(address_info{t->get_attr<int32_t>("ddr_addr"), 
            t->get_data_size(), layer_info::name_map(t->get_name())});
      }
      auto out_tensors = (*child)->get_output_tensors() ;
      for(auto t: out_tensors) {
        layer.outputs.emplace_back(address_info{t->get_attr<int32_t>("ddr_addr"), 
            t->get_data_size(), layer_info::name_map(t->get_name())});
      } 

      dbg_layers_.emplace_back(std::move(layer));
    }

  }

  // reg0
  if (parameter_size) {
    void *reg0Ptr = NULL; 
    if (posix_memalign(&reg0Ptr, getpagesize(), parameter_size))
      throw std::bad_alloc();
    for (unsigned i=0; i < parameter_size; i++) ((char*)reg0Ptr)[i] = parameter_value[i];
    auto reg0Mem 
      = xclAllocUserPtrBO(handle, reg0Ptr, parameter_size, handle_->get_device_info().ddr_bank);
    xclSyncBO(handle, reg0Mem, XCL_BO_SYNC_BO_TO_DEVICE, parameter_size, 0);
    xclGetBOProperties(handle, reg0Mem, &boProp);
    reg0_addr_ = boProp.paddr;
    free(reg0Ptr);
  } else {
    reg0_addr_ = 0;
  }
  program_once_complete = 0;
}

std::vector<float> DpuV4eController::get_input_scale() {
  return input_scales_;
}

std::vector<float> DpuV4eController::get_output_scale() {
  return output_scales_;
}


std::vector<const xir::Tensor*> 
DpuV4eController::get_input_tensors() const {
  return input_tensors_;
}

std::vector<const xir::Tensor*> 
DpuV4eController::get_output_tensors() const {
  return output_tensors_;
}

std::vector<const xir::Tensor*> 
DpuV4eController::get_merged_io_tensors() const {
  const std::vector<std::int32_t> dims = { 1, 1, 1, xdpu_io_total_size };
  xir::Tensor *tensor = xir::Tensor::create("inout", dims, xir::DataType{xir::DataType::INT, 8}).release();
  //static xir::Tensor tensor("inout", dims, xir::Tensor::DataType::INT8); 
  return std::vector<const xir::Tensor*>(8, tensor);
}

std::vector<vart::TensorBuffer*> get_tensor_buffer_pointer(
    std::vector<std::unique_ptr<vart::TensorBuffer>>& tensor_buffers) {
  auto ret = std::vector<vart::TensorBuffer*>();
  ret.reserve(tensor_buffers.size());
  std::transform(tensor_buffers.begin(), tensor_buffers.end(),
                 std::back_inserter(ret),
                 [](auto& tensor_buffer) { return tensor_buffer.get(); });
  return ret;
}
std::vector<vart::TensorBuffer*> DpuV4eController::get_inputs() {
//  return get_tensor_buffer_pointer(input_tensor_buffers_);
  return init_tensor_buffer(input_tensors_);
}

std::vector<vart::TensorBuffer*> DpuV4eController::get_outputs() {
//  return get_tensor_buffer_pointer(output_tensor_buffers_);
  auto tbufs = init_tensor_buffer(output_tensors_);
  auto hwbufs = create_tensor_buffers(get_merged_io_tensors(),false);
  for (int i=0;i<BATCHSIZE; i++) {
    std::unique_lock<std::mutex> lock(hwbuf_mtx_);
    tbuf2hwbuf_.emplace(tbufs[i*output_tensors_.size()], hwbufs[i]);
    
  }
  return tbufs;
}

void DpuV4eController::data_fix2float(float* dataDst, int8_t* dataSrc, int size, float scale) {
  for (int i = 0; i < size; i++)
    dataDst[i] = (float)(dataSrc[i]*scale);
}

void DpuV4eController::data_float2fix(int8_t* dataDst, float* dataSrc, int size, float scale) {
  for (int i = 0; i < size; i++)
    dataDst[i] = (int8_t)(dataSrc[i]*scale);
}
void DpuV4eController::free_buffers(std::vector<vart::TensorBuffer*> &tbufs, bool isInput) {
  if (isInput) {
    std::unique_lock<std::mutex> lock(hwbuf_mtx_);
    for (unsigned ti=0; ti < tbufs.size(); ti++)
    {
      for (auto it=bufs_.begin(); it != bufs_.end(); it++)
        if (it->get() == tbufs[ti])
        {
          bufs_.erase(it);
          break;
        }
    }

  } else {
    std::unique_lock<std::mutex> lock(hwbuf_mtx_);
    for (unsigned ti=0; ti < tbufs.size(); ti++)
    {
      {
        std::unique_lock<std::mutex> lock(tbuf_mtx_);
        tbuf2dbuf_.erase(tbuf2hwbuf_[tbufs[ti]]);
        for (auto it=tbufs_.begin(); it != tbufs_.end(); it++)
          if (it->get() == tbuf2hwbuf_[tbufs[ti]])
          {
             tbufs_.erase(it);
             break;
          }

      }
      tbuf2hwbuf_.erase(tbufs[ti]);
      for (auto it=bufs_.begin(); it != bufs_.end(); it++)
        if (it->get() == tbufs[ti])
        {
           bufs_.erase(it);
           break;
        }
    }
  }
}

void DpuV4eController::run(const std::vector<vart::TensorBuffer*> &inputs, 
    const std::vector<vart::TensorBuffer*> &outputs) {
  std::vector<vart::TensorBuffer*> input_tensor_buffers;
  std::vector<vart::TensorBuffer*> output_tensor_buffers;
  if(inputs.size()%input_tensors_.size())
    throw std::runtime_error("Error: input tensorbuffers error");
  unsigned ibs = inputs[0]->get_tensor()->get_shape()[0]/input_tensors_[0]->get_shape()[0];
  unsigned obs = outputs[0]->get_tensor()->get_shape()[0]/output_tensors_[0]->get_shape()[0];
  unsigned inputBs;
  // check if tensorbuffer store batch inputs/outputs
  if ((inputs.size()/input_tensors_.size())>1)
    inputBs = inputs.size()/input_tensors_.size();
  else
    inputBs = ibs;
  if ((ibs < obs) || (inputBs > BATCHSIZE) )
    throw std::runtime_error("Error: size of tensorbuffer not supported");
  bool create_tb_outside=false;
  auto it = tbuf2hwbuf_.find(outputs[0]);
  if (it == tbuf2hwbuf_.end())
  {
    create_tb_outside=true;
  }
  if(create_tb_outside) {
    input_tensor_buffers = get_inputs();
    output_tensor_buffers = get_outputs();
    for (unsigned i=0; i < input_tensors_.size(); i++ ) {
      unsigned cnt=0;
      for (unsigned j=0; j < inputs.size(); j++) {
        if (input_tensors_[i]->get_name().find(inputs[j]->get_tensor()->get_name()) != std::string::npos) {
          if (ibs == inputBs) { //one tensrobuffer store batch
            for (unsigned b=0; b < ibs; b++) {
              if (inputs[j]->get_tensor()->get_data_type().type == xir::DataType::FLOAT)
                data_float2fix((int8_t*)input_tensor_buffers[b*input_tensors_.size()+i]->data().first,(float*)inputs[j]->data().first+b*input_tensors_[i]->get_element_num(),input_tensors_[i]->get_element_num(), input_scales_[i]);
              else
                memcpy((int8_t*)input_tensor_buffers[b*input_tensors_.size()+i]->data().first,(int8_t*)inputs[j]->data().first+b*input_tensors_[i]->get_element_num(),input_tensors_[i]->get_element_num());
              cnt++;
            }
          }
          else {
            if (inputs[j]->get_tensor()->get_data_type().type == xir::DataType::FLOAT)
              data_float2fix((int8_t*)input_tensor_buffers[cnt*input_tensors_.size()+i]->data().first,(float*)inputs[j]->data().first,input_tensors_[i]->get_element_num(), input_scales_[i]);
            else
              memcpy((int8_t*)input_tensor_buffers[cnt*input_tensors_.size()+i]->data().first,(int8_t*)inputs[j]->data().first,input_tensors_[i]->get_element_num());
            cnt++;
          }

        }
        
      }
      if (cnt == 0)
        throw std::runtime_error("Error: invilad tensorbuffer input");
    }
  }
  else {
    input_tensor_buffers = inputs;
    output_tensor_buffers = outputs;
  }

  Engine& engine = Engine::get_instance();
  const unsigned worker_id = engine.get_my_worker_id();
  if (worker_id >= contexts_.size())
    throw std::runtime_error("Error: worker_id too large; update controller code");

  auto &context = *(contexts_[worker_id]);
  auto xcl_handle = context.get_dev_handle();
  auto bo_handle = context.get_bo_handle();
  auto bo_addr = context.get_bo_addr();

  // get device buffers for input TensorBuffers
  std::vector<XrtDeviceBuffer*> io_bufs(inputBs);
  std::vector<uint64_t> io_addrs(inputBs);
  for (unsigned i=0; i < inputBs; i++)
  {
    vart::TensorBuffer* hwbuf;
    {
      std::unique_lock<std::mutex> lock(hwbuf_mtx_);
      auto it = tbuf2hwbuf_.find(output_tensor_buffers[i*output_tensors_.size()]);
      if (it == tbuf2hwbuf_.end())
        throw std::runtime_error("TensorBuffer not found");
      hwbuf = it->second;

    }
    io_bufs[i] = dynamic_cast<XrtDeviceBuffer*>(get_device_buffer(hwbuf));
    io_addrs[i] = io_bufs[i]->get_phys_addr(); 
  }

  // upload batch of inputs
  //const auto inSize = get_input_tensors()[0]->get_element_num();
  __TIC__(INPUT_H2D)
  for (unsigned i=0; i < io_bufs.size(); i++)
  {
    // instead of uploading all {output, input, intermediate}, 
    // just upload input region
    // io_bufs[i]->upload();
   //const auto mode = std::ios_base::out | std::ios_base::binary | std::ios_base::trunc;
   //  auto input_file = "./input"+ to_string(i)+".bin";
   //  std::ofstream(input_file, mode).write((char*)inputs[i]->data().first,inSize);

    for (unsigned j=0; j < xdpu_io_input_offset.size(); j++) {
      const auto inSize = get_input_tensors()[j]->get_element_num();
      if (xclUnmgdPwrite(xcl_handle, 0, (void *)input_tensor_buffers[i*xdpu_io_input_offset.size()+j]->data().first, inSize,
      //if (xclUnmgdPwrite(xcl_handle, 0, inputs[i]->data().first, inSize,
        io_addrs[i] + xdpu_io_input_offset[j]))
        throw std::runtime_error("Error: upload failed");
    }
 
  }
  __TOC__(INPUT_H2D)

  auto ecmd = reinterpret_cast<ert_start_kernel_cmd*>(bo_addr);
  ecmd->cu_mask = handle_->get_device_info().cu_mask;
  ecmd->extra_cu_masks = 0;
  ecmd->stat_enabled = 1;
  ecmd->state = ERT_CMD_STATE_NEW;
  ecmd->opcode = ERT_EXEC_WRITE;
  ecmd->type = ERT_CTRL;


  auto trigger_dpu_func = [&](){ 

    std::vector<std::pair<int, int> > regVals;
    regVals.push_back({ XDPU_CONTROL_AP, XDPU_CONTROL_AP_START });
    regVals.push_back({ XDPU_CONTROL_GIE / 4, XDPU_GLOBAL_INT_ENABLE });
    regVals.push_back(  { XDPU_CONTROL_IER / 4, 1 });
    regVals.push_back(  { XDPU_CONTROL_START / 4, 0x0 });
    regVals.push_back(  { XDPU_CONTROL_RESET / 4, 0x1 });
    regVals.push_back(  { XDPU_CONTROL_HP / 4, 0x204040 });
    regVals.push_back(  { XDPU_CONTROL_INSTR_L / 4, code_addr_ & 0xFFFFFFFF });
    regVals.push_back(  { XDPU_CONTROL_INSTR_H / 4, (code_addr_ >> 32) & 0xFFFFFFFF });
    regVals.push_back(  { XDPU_CONTROL_ADDR_0_L / 4, reg0_addr_ & 0xFFFFFFFF });
    regVals.push_back(  { XDPU_CONTROL_ADDR_0_H / 4, (reg0_addr_ >> 32) & 0xFFFFFFFF });
 

    // program DPU input/output addrs
    for (unsigned i=0; i < io_addrs.size(); i++) {
      auto lowOffset = (XDPU_CONTROL_ADDR_1_L + (i*0x100)) / 4;
      auto highOffset = (XDPU_CONTROL_ADDR_1_H + (i*0x100)) / 4;
      regVals.push_back({ lowOffset, io_addrs[i] & 0xFFFFFFFF });
      regVals.push_back({ highOffset, (io_addrs[i] >> 32) & 0xFFFFFFFF });
    }

 
    int p = 6;
    for (unsigned i=0; i < regVals.size(); i++) {
      ecmd->data[p++] = (regVals[i].first) * 4;
      ecmd->data[p++] = (regVals[i].second);
    }
    ecmd->count = 1 + p; 

    // exec kernel
    auto exec_buf_result = xclExecBuf(xcl_handle, bo_handle);
    if (exec_buf_result)
      throw std::runtime_error("Error: xclExecBuf failed");

    // wait for kernel
    for (int wait_count=0; wait_count < 15 && xclExecWait(xcl_handle, 1000) == 0 
            && ecmd->state != ERT_CMD_STATE_COMPLETED; wait_count++);
            
    if (ecmd->state != ERT_CMD_STATE_COMPLETED) {
      std::cout << "Error: CU timeout " << std::endl;

      std::cout << "LOAD START:" << read32_dpu_reg(xcl_handle,  0+ DPUREG_LOAD_START) << std::endl;
      std::cout << "LOAD END  :" << read32_dpu_reg(xcl_handle,  0+ DPUREG_LOAD_END) << std::endl;
      std::cout << "SAVE START:" << read32_dpu_reg(xcl_handle,  0+ DPUREG_SAVE_START) << std::endl;
      std::cout << "SAVE END  :" << read32_dpu_reg(xcl_handle,  0+ DPUREG_SAVE_END) << std::endl;
      std::cout << "CONV START:" << read32_dpu_reg(xcl_handle,  0+ DPUREG_CONV_START) << std::endl;
      std::cout << "CONV END  :" << read32_dpu_reg(xcl_handle,  0+ DPUREG_CONV_END) << std::endl;
      std::cout << "MISC START:" << read32_dpu_reg(xcl_handle,  0+ DPUREG_MISC_START) << std::endl;
      std::cout << "MISC END  :" << read32_dpu_reg(xcl_handle,  0+ DPUREG_MISC_END) << std::endl;
      throw std::runtime_error("Error: CU timeout " + std::to_string(handle_->get_device_info().cu_index));
    }
    if(ENV_PARAM(DPU_IP_LATENCY))
      std::cout << "IP COUNTER:" << read32_dpu_reg(xcl_handle, 0+DPUREG_CYCLE_COUNTER) <<std::endl;
  // download results into output TensorBuffers
  };

  // program DPU request
  if(!debug_mode_) { //=== run release instructions 
    if(dump_mode_ ) { // dump input    
      int tensor_idx = 0;
      for(auto& out: dbg_layers_[0].inputs) {
        auto offset = std::get<0>(out);
        auto size = std::get<1>(out);
        auto data = std::make_unique<char[]>(size);
        for (unsigned i=0; i < io_bufs.size(); i++) { 
          if (xclUnmgdPread(xcl_handle, 0, data.get(), size, io_addrs[i] + offset))
            throw std::runtime_error("Error: dump failed!");
          std::stringstream ss;  
          ss << dump_folder_ << "/E" << i << "/" << std::get<2>(out); 
          std::ofstream ofs(ss.str(), std::ofstream::binary);
          ofs.write(data.get(), size);
          ofs.close(); 
        }
        tensor_idx++;
      } 
    }

    trigger_dpu_func(); 

    if(dump_mode_ ) {  // dump final output   
      int tensor_idx = 0;
      for(auto& out: dbg_layers_[0].outputs) {
        auto offset = std::get<0>(out);
        auto size = std::get<1>(out);
        auto data = std::make_unique<char[]>(size);
        for (unsigned i=0; i < io_bufs.size(); i++) { 
          if (xclUnmgdPread(xcl_handle, 0, data.get(), size, io_addrs[i] + offset))
            throw std::runtime_error("Error: dump failed!");
          std::stringstream ss;  
          ss << dump_folder_ << "/E" << i << "/" << std::get<2>(out); 
          std::ofstream ofs(ss.str(), std::ofstream::binary);
          ofs.write(data.get(), size);
          ofs.close(); 
        }
        tensor_idx++;
      } 
    }
  } else {//=== run debug instructions
    // dump first layer's inputs
    if(dump_mode_ && (dbg_layers_.size() > 0)) {   
      auto& inputs = dbg_layers_[0].inputs;
      int tensor_idx = 0;
      for(auto& input: inputs) {
        auto offset = std::get<0>(input);
        auto size = std::get<1>(input);
        auto data = std::make_unique<char[]>(size);
        for (unsigned i=0; i < io_bufs.size(); i++) { 
          if (xclUnmgdPread(xcl_handle, 0, data.get(), size, io_addrs[i] + offset))
            throw std::runtime_error("Error: dump failed!");
          std::stringstream ss;
          ss << dump_folder_ << "/E" << i << "/" << std::get<2>(input); 
          std::ofstream ofs(ss.str(), std::ofstream::binary);
          ofs.write(data.get(), size);
          ofs.close(); 
        }
        tensor_idx++;
      } 
    }

    int layer_idx = 0; 
    for(auto iter = dbg_layers_.begin() + 1;iter !=dbg_layers_.end();iter++) {
      auto layer = *iter;
      if(std::get<1>(layer.code_addr) > 0) {
        code_addr_ = std::get<0>(layer.code_addr); 
        trigger_dpu_func();  
      }
      // Save the outputs to file  
      if(dump_mode_) { 
        int tensor_idx = 0;
        for(auto& out: layer.outputs) {
          auto offset = std::get<0>(out);
          auto size = std::get<1>(out);
          auto data = std::make_unique<char[]>(size);
          for (unsigned i=0; i < io_bufs.size(); i++) { 
            if (xclUnmgdPread(xcl_handle, 0, data.get(), size, io_addrs[i] + offset))
              throw std::runtime_error("Error: dump failed!");
            std::stringstream ss;  
            ss << dump_folder_ << "/E" << i << "/" << std::get<2>(out); 
            std::ofstream ofs(ss.str(), std::ofstream::binary);
            ofs.write(data.get(), size);
            ofs.close(); 
          }
          tensor_idx++;
        }
      }
      layer_idx ++;
    } 
  }

  __TIC__(OUTPUT_D2H)
  for (unsigned i=0; i < io_bufs.size(); i++)
  {
    // instead of downloading all {output, input, intermediate},
    // just download output region
    // io_bufs[i]->download();
    int sumSize=0;
    for (unsigned j=0; j< xdpu_io_output_offset.size(); j++) {
      const auto outSize = get_output_tensors()[j]->get_element_num();
      //__TIC_PROFILING__(OUTPUT)
      if (xclUnmgdPread(xcl_handle, 0, (void*)output_tensor_buffers[i*xdpu_io_output_offset.size()+j]->data().first,
        outSize,
        io_addrs[i] + xdpu_io_output_offset[j]))
        throw std::runtime_error("Error: download failed");
      //__TOC_PROFILING__(OUTPUT)
      sumSize += outSize;
    }
  }
  __TOC__(OUTPUT_D2H)
  if(create_tb_outside) {
    for (unsigned i=0; i < output_tensors_.size(); i++  ) {
      unsigned cnt=0;
      for (unsigned j=0; j < outputs.size(); j++) {
        if (output_tensors_[i]->get_name().find(outputs[j]->get_tensor()->get_name()) != std::string::npos) {
          if (ibs == inputBs) {
            for (unsigned b=0; b < obs; b++) {
              if (outputs[j]->get_tensor()->get_data_type().type == xir::DataType::FLOAT)
                data_fix2float((float*)outputs[j]->data().first+b*output_tensors_[i]->get_element_num(), (int8_t*)output_tensor_buffers[b*output_tensors_.size()+i]->data().first,output_tensors_[i]->get_element_num(),output_scales_[i]);
              else
                memcpy((char*)outputs[j]->data().first+b*output_tensors_[i]->get_element_num(), (void*)output_tensor_buffers[b*output_tensors_.size()+i]->data().first,output_tensors_[i]->get_element_num());
              cnt++;
            }
          }
          else {
            if (outputs[j]->get_tensor()->get_data_type().type == xir::DataType::FLOAT)
              data_fix2float((float*)outputs[j]->data().first,(int8_t*)output_tensor_buffers[cnt*output_tensors_.size()+i]->data().first,output_tensors_[i]->get_element_num(),output_scales_[i]);
            else
              memcpy((int8_t*)outputs[j]->data().first,(int8_t*)output_tensor_buffers[cnt*output_tensors_.size()+i]->data().first,output_tensors_[i]->get_element_num());
            cnt++;

          }

        }
      }
      if (cnt == 0)
        throw std::runtime_error("Error: invilad tensorbuffer output");
    }
    free_buffers(input_tensor_buffers,true);
    free_buffers(output_tensor_buffers,false);
  }
}

