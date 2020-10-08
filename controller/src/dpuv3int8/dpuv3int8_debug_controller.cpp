#ifndef BATCH_SIZE
#define BATCH_SIZE 4
#endif
#include "dpuv3int8_controller.hpp"

using namespace std;

Dpuv3Int8DebugController::Dpuv3Int8DebugController(std::string meta) : Dpuv3Int8Controller(meta) {
  
  xmodel_.reset(new Xmodel(meta, true));
  loadBinFile(xmodel_->getDebugDinFilename(), true);
  loadBinFile(xmodel_->getDebugGoldenFilename(), false);
 if(not xmodel_->getSinglePoolDebug())
  {
    debugDumpParams((void*)params_.data(), params_.size()*BATCH_SIZE);
    debugDumpParamsDdrFormat();
  }

  debugDumpInstr();
  debugDumpInstrAcCode(); 

}

void Dpuv3Int8DebugController::debugDumpInstrAcCode()
{
  debug_dumpvals_.open(xmodel_->getDebugDumpdir()+"instrasmUsed.asm");
  debug_dumpvals_<<"*****************************************************\n";
  debug_dumpvals_<<"This section contains compiler instructions in asm format\n";
  std::ifstream inputFile;
  std::string instrLine;
  inputFile.open(xmodel_->getInstrAsmFileName());
  while(std::getline(inputFile, instrLine))
  {
    debug_dumpvals_<<instrLine<<"\n";
  }
  
  debug_dumpvals_.close();

}

void Dpuv3Int8DebugController::debugDumpInstr()
{

  debug_dumpvals_.open(xmodel_->getDebugDumpdir()+"instrDdrFormatUsed.txt");
  debug_dumpvals_<<"*****************************************************\n";
  debug_dumpvals_<<"This section contains compiler instructions in ddr format, contents of instr.txt\n";
  std::ifstream   ifile;
  std::string     line;
  ifile.open(xmodel_->getInstrFileName());
  while(std::getline(ifile, line))
  {
      debug_dumpvals_<<line.c_str()<<"\n";
  }
  ifile.close();
  
  debug_dumpvals_.close();

}
void Dpuv3Int8DebugController::debugDumpParamsDdrFormat()
{
  debug_dumpvals_.open(xmodel_->getDebugDumpdir()+"paramsUsed.txt");
  debug_dumpvals_<<"*****************************************************\n";
  debug_dumpvals_<<"This section contains contents of params.txt in ddr format\n";
  std::ifstream   ifile;
  std::string     line;
  ifile.open(xmodel_->getInstrFileName());
  while(std::getline(ifile, line))
  {
      debug_dumpvals_<<line.c_str()<<"\n";
  }
  ifile.close();
  
  debug_dumpvals_.close();

}


void Dpuv3Int8DebugController::debugDumpParams(void *params_data, int params_size)
{
      std::vector<int8_t> paramsData(params_size,0);
      for(uint32_t k=0; k<paramsData.size(); k++)
      {
        paramsData[k]=*(int8_t *)((long long) params_data+k);
      }

      convert2DmemFormat(paramsData, "params", xmodel_->getDebugDumpdir()+"paramsdmem.txt");

}


void Dpuv3Int8DebugController::preprocess(vart::TensorBuffer* stdbuf, vart::TensorBuffer* hwbuf)
{
    if(xmodel_->getChannelAugmentationMode())
      channelAug();
    if(not xmodel_->getDruMode())
    {
      batchInterleave();
      memcpy((void*)hwbuf->data().first, (void*)debugBatchInterleaved_.data(), (debugBatchInterleaved_.size()));
    }
    else
    {
      void* std_data = (void*)stdbuf->data().first;
      std::vector<uint8_t> flattenedData(xmodel_->getInW()*xmodel_->getInH()*xmodel_->getInCh()*BATCH_SIZE,0);
      for(int i=0; i<flattenedData.size(); i++)
      {
        flattenedData[i]=*(int8_t *)((long long) std_data+i);
      }
  
      std::vector<int,aligned_allocator<int>> hwDinVector(flattenedData.size()/4,0);
  
      int j=0;
      
      for(int i=0; i<flattenedData.size(); i=i+BATCH_SIZE)
      {
        uint8_t *ptr = (uint8_t*)&(hwDinVector[j]);
        for(int o=0; o<BATCH_SIZE; o++)
        {
          ptr[o] = flattenedData[i+o];
  
        }
        j++;
      }
      
      memcpy((void*)hwbuf->data().first, (void*)hwDinVector.data(), hwDinVector.size()*BATCH_SIZE);

    }

}

void Dpuv3Int8DebugController::output_reorg(void *std_data, void *result_data, int result_size)
{
    int i, mbatch, segment, group;
    std::vector<int> count(BATCH_SIZE,0); 

    for (i = 0; i < result_size; i++)
    {
        mbatch = i / (64*16*xmodel_->getOutSize());
        segment = (i - mbatch * (64*16*xmodel_->getOutSize())) / 64;
        group = (i - mbatch * (64 * 16*xmodel_->getOutSize()) - segment * 64) / 16;
        switch(mbatch*4+group)
        {
           case 0: *(int8_t *)((long long) std_data+count[0])=*(int8_t *)((long long)result_data+i);
                   count[0]++;
                   break;

           case 1: *(int8_t *)((long long) std_data+(xmodel_->getOutSize()+count[1]))=*(int8_t *)((long long)result_data+i);
                   count[1]++;
                   break;
           case 2: *(int8_t *)((long long) std_data+(xmodel_->getOutSize()*2+count[2]))=*(int8_t *)((long long)result_data+i);
                    count[2]++;
                    break;
           case 3: *(int8_t *)((long long) std_data+(xmodel_->getOutSize()*3+count[3]))=*(int8_t *)((long long)result_data+i);
                   count[3]++;
        }
        
    }
    
    debugDumpOutputs(std_data, result_data, result_size);
    compareAgainstGolden(std_data);
}


void Dpuv3Int8DebugController::initRunBufs(uint64_t *buf_addr, XclDeviceBuffer* swap_buf, XclDeviceBuffer* druSrc_buf, XclDeviceBuffer* druDst_buf)
{
    buf_addr[BUF_IDX_INSTR] = instr_buf_->get_phys_addr();
    
    if(not xmodel_->getSinglePoolDebug())
      buf_addr[BUF_IDX_PARAMS] = params_buf_->get_phys_addr();
    
    buf_addr[BUF_IDX_SWAP] = swap_buf->get_phys_addr();

    buf_addr[BUF_IDX_DRUSRC] = druSrc_buf->get_phys_addr();
    buf_addr[BUF_IDX_DRUDST] = druDst_buf->get_phys_addr();

    if(xmodel_->getDruMode()) // If DRU is used 
    {
        buf_addr[BUF_IDX_DST] = buf_addr[BUF_IDX_DRUDST];
    }                                                                 
    else            // If DRU is NOT used
    {                                                                 
        buf_addr[BUF_IDX_SRC] = buf_addr[BUF_IDX_NULL];
    }

}


void Dpuv3Int8DebugController::runKernel(xrtcpp::exec::exec_write_command cmd, uint64_t* buf_addr, uint32_t* reg_val)
{

    cmd.add(CONTROL_ADDR_BLOCK_INSTR ,(buf_addr[BUF_IDX_INSTR]) & 0xFFFFFFFF);
    cmd.add(CONTROL_ADDR_BLOCK_INSTR + 0x4 ,((buf_addr[BUF_IDX_INSTR]) >> 32) & 0xFFFFFFFF);
    
    if(not xmodel_->getSinglePoolDebug())
    {
      cmd.add(CONTROL_ADDR_BLOCK_PARAMS ,(buf_addr[BUF_IDX_PARAMS]) & 0xFFFFFFFF);
      cmd.add(CONTROL_ADDR_BLOCK_PARAMS + 0x4 ,((buf_addr[BUF_IDX_PARAMS]) >> 32) & 0xFFFFFFFF);
    }
    
    cmd.add(CONTROL_ADDR_BLOCK_SWAP ,(buf_addr[BUF_IDX_SWAP]) & 0xFFFFFFFF);
    cmd.add(CONTROL_ADDR_BLOCK_SWAP + 0x4 ,((buf_addr[BUF_IDX_SWAP]) >> 32) & 0xFFFFFFFF);
    cmd.add(CONTROL_ADDR_BLOCK_RSLT ,(buf_addr[BUF_IDX_RESULT]) & 0xFFFFFFFF);
    cmd.add(CONTROL_ADDR_BLOCK_RSLT + 0x4 ,((buf_addr[BUF_IDX_RESULT]) >> 32) & 0xFFFFFFFF);
    cmd.add(CONTROL_ADDR_BLOCK_SRC ,(buf_addr[BUF_IDX_SRC]) & 0xFFFFFFFF);
    cmd.add(CONTROL_ADDR_BLOCK_SRC + 0x4 ,((buf_addr[BUF_IDX_SRC]) >> 32) & 0xFFFFFFFF);
    cmd.add(CONTROL_ADDR_BLOCK_DST ,(buf_addr[BUF_IDX_DST]) & 0xFFFFFFFF);
    cmd.add(CONTROL_ADDR_BLOCK_DST + 0x4 ,((buf_addr[BUF_IDX_DST]) >> 32) & 0xFFFFFFFF);

    cmd.add(CONTROL_ADDR_TASK_DRU_ADDR_STRD ,reg_val[REG_IDX_TASK_DRU_ADDR_STRD]);
    cmd.add(CONTROL_ADDR_TASK_DRU_KW ,reg_val[REG_IDX_TASK_DRU_KW]);
    cmd.add(CONTROL_ADDR_TASK_DRU_SW ,reg_val[REG_IDX_TASK_DRU_SW]);
    cmd.add(CONTROL_ADDR_TASK_DRU_IC ,reg_val[REG_IDX_TASK_DRU_IC]);
    cmd.add(CONTROL_ADDR_TASK_DRU_OW ,reg_val[REG_IDX_TASK_DRU_OW]);
    cmd.add(CONTROL_ADDR_TASK_DRU_OH ,reg_val[REG_IDX_TASK_DRU_OH]);
    cmd.add(CONTROL_ADDR_TASK_DRU_SRC_NTRANS ,reg_val[REG_IDX_TASK_DRU_SRC_NTRANS]);
    cmd.add(CONTROL_ADDR_TASK_DRU_DST_NTRANS ,reg_val[REG_IDX_TASK_DRU_DST_NTRANS]);
    cmd.add(CONTROL_ADDR_TASK_DRU_PL_CORR	,reg_val[REG_IDX_TASK_DRU_PL_CORR]);
    cmd.add(CONTROL_ADDR_TASK_DRU_PR_CORR	,reg_val[REG_IDX_TASK_DRU_PR_CORR]);
    cmd.add(CONTROL_ADDR_TASK_DRU_IW_CORR	,reg_val[REG_IDX_TASK_DRU_IW_CORR]);
    cmd.add(CONTROL_ADDR_TASK_DRU_SW_CORR	,reg_val[REG_IDX_TASK_DRU_SW_CORR]);
    cmd.add(CONTROL_ADDR_TASK_DRU_WCG_CORR ,reg_val[REG_IDX_TASK_DRU_WCG_CORR]);
    cmd.add(CONTROL_ADDR_TASK_DRU_READ_MODE ,reg_val[REG_IDX_TASK_DRU_READ_MODE]);
    cmd.add(CONTROL_ADDR_TASK_MODE ,reg_val[REG_IDX_TASK_MODE]);
    cmd.add(CONTROL_ADDR_REG_AXCACHE_AXOS	,reg_val[REG_IDX_REG_AXCACHE_AXOS]);
   
    cmd.add(CONTROL_ADDR_REG_DPU_PROF_ENABLE ,reg_val[REG_IDX_REG_DPU_PROF_ENABLE]);
    debugDumpRegVals(buf_addr, reg_val);

    auto t1 = std::chrono::high_resolution_clock::now();
    cmd.execute();
    cmd.wait();
    auto t2 = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = t2-t1;
    std::cout << "KernelTime(millisec): " << elapsed.count()*1000 << std::endl;



}

std::string Dpuv3Int8DebugController::toHexWidth2( uint32_t i )
{
  std::stringstream stream;
  stream << std::setfill ('0') << std::setw(2) 
         << std::hex << i;
  return stream.str();
}

std::string Dpuv3Int8DebugController::toHexWidth16( uint32_t i )
{
  std::stringstream stream;
  stream << std::setfill ('0') << std::setw(16) 
         << std::hex << i;
  return stream.str();
}

void Dpuv3Int8DebugController::convert2DmemFormat(std::vector<int8_t> &flattenedData, std::string dataName, std::string outFileName)
{

  std::vector<std::string> dwData(flattenedData.size()/64,"");
  std::string data_s = "";
  std::string dw = "";
  int p=0;
  int baseAddr = 0;
  if(dataName!="input")
    if(dataName!="inputBatchInterleaved")
      if(dataName=="params")
        baseAddr = 805306368;
      else
        baseAddr = 268435456;//basically we use dmem conversion only for inputs and outputs, for inputs we set base addr as 0x0 and for outputs we want to set base addr as 0x10000000, since 0x10000000 is what is used in simulation reference model
  for(int i=0; i<flattenedData.size(); i=i+64)
  {
    data_s = "";
    for(int j=0; j<64; j++)
    {
      data_s = toHexWidth2(flattenedData[i+j] & 0xff)+data_s;
    }
    dw = "";
    dw = toHexWidth16(i+baseAddr);
    dwData[p]="0x"+dw+" : "+data_s;
    p=p+1;
  }

  std::ofstream oFile;
  oFile.open(outFileName);

  oFile<<"dmem format of "<<dataName<<"\n";

  for(int k=0; k<dwData.size(); k++)
  {
    oFile<<dwData[k]<<"\n";
  }

}

void Dpuv3Int8DebugController::loadBinFile(std::string binFileName, bool isInput)
{
  std::ifstream stream(binFileName, std::ios::in | std::ios::binary);  
  std::vector<int8_t> contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());

  if(isInput)
  {
    debugInput_.resize(contents.size());
    debugInput_=contents;
    convert2DmemFormat(debugInput_, "input", xmodel_->getDebugDumpdir()+"inputDmem.txt");

    std::cout<<"Number of input int8 values: "<<debugInput_.size()<<std::endl;

  }
  else
  {
    debugGolden_.resize(contents.size());
    debugGolden_=contents;
    
    debug_dumpvals_.open(xmodel_->getDebugDumpdir()+"goldenFlattened.txt");
    for(int p=0; p<debugGolden_.size(); p++)
    {
      debug_dumpvals_<<"idx: "<<p<<" int8 val: "<<(int32_t)debugGolden_[p]<<"\n";
    }
    debug_dumpvals_.close();
    
    convert2DmemFormat(debugGolden_, "golden output", xmodel_->getDebugDumpdir()+"goldenOutputDmem.txt");
    
    std::cout<<"Number of golden output int8 values: "<<debugGolden_.size()<<std::endl;
  }

}

void Dpuv3Int8DebugController::compareAgainstGolden(void *outData)
{
    //xir::vart::TensorBuffer* tbuf = outbuf->get_tensor_buffer();
    
    debug_dumpvals_.open(xmodel_->getDebugDumpdir()+"comparisonAgsintGoldenOut.txt");
    debug_dumpvals_<<"*****************************************************\n";
      debug_dumpvals_<<"This section prints whether or not outputs in standard NHWC format match the golden binary file provided, it also prints out int8 values along with indices, of results that mismatch\n";

    
    int outSize = xmodel_->getOutSize()*BATCH_SIZE;
//    void *outData = (void*)tbuf->data().first;
    
    if(debugGolden_.size()!=outSize)
    {
       debug_dumpvals_<<"SIZE MISMATCH: Outputs from board converted to std format have size: "<<outSize<<" and Golden Outputs have size: "<<debugGolden_.size()<<"\n";
       debug_dumpvals_.close();
       std::cout<<"ERROR! OUTPUT SIZE MISMATCH: Outputs from board converted to std format have size: "<<outSize<<" and Golden Outputs have size: "<<debugGolden_.size()<<std::endl;
       exit(1);
    }
    
    int counter = 0;
    long double difference = 0;
    for(int j=0; j<outSize; j++)
    {
         
      if((int32_t)*(int8_t *)((long long)outData+j)!=(int32_t)debugGolden_[j])
      {
         debug_dumpvals_<<"Mismatch at idx: "<<j<<" int8 board std value: "<<(int32_t)*(int8_t *)((long long)outData+j)<<" int8 golden output value: "<<(int32_t)debugGolden_[j]<<"\n"; 
         difference+=((int32_t)*(int8_t *)((long long)outData+j)-(int32_t)debugGolden_[j])*((int32_t)*(int8_t *)((long long)outData+j)-(int32_t)debugGolden_[j]);
         counter++;
      }

    }

    difference = sqrt(difference);
    if(counter==0)
    {
      debug_dumpvals_<<"RESULT: SUCCESS, All outputs from board converted to std format match perfectly with golden!\n";
      std::cout<<"RESULT: SUCCESS, All outputs from board converted to std format match perfectly with golden!"<<std::endl;
    }
    else
    {
      debug_dumpvals_<<"L2 difference: "<<difference<<"\n";
      debug_dumpvals_<<"RESULT: FAIL, "<<counter<<" number of values mismatch between board outputs converted to std format and golden values\n";
      std::cout<<"L2 difference: "<<difference<<std::endl;
      std::cout<<"RESULT: FAIL, "<<counter<<" number of values mismatch between board outputs converted to std format and golden values"<<std::endl;
    }
    debug_dumpvals_.close();

}

void Dpuv3Int8DebugController::batchInterleave()
{

  debug_dumpvals_.open(xmodel_->getDebugDumpdir()+"inputsStd.txt");
  debug_dumpvals_<<"*****************************************************\n";
  if(xmodel_->getChannelAugmentationMode())
  {
    debug_dumpvals_<<"This section contains int8 input values before batch interleave, after channel augmentation\n";
    for(int i=0; i<debugChAug_.size(); i++)
    {
      debug_dumpvals_<<"idx: "<<i<<" int8 val: "<<(int32_t)debugChAug_[i]<<"\n";
    }
  }
  else
  {
    debug_dumpvals_<<"This section contains int8 input values before batch interleave\n";
    for(int i=0; i<debugInput_.size(); i++)
    {
      debug_dumpvals_<<"idx: "<<i<<" int8 val: "<<(int32_t)debugInput_[i]<<"\n";
    }
  }
  debug_dumpvals_.close();

  debug_dumpvals_.open(xmodel_->getDebugDumpdir()+"inputBatchInterleaved.txt");
  debug_dumpvals_<<"*****************************************************\n";
  debug_dumpvals_<<"This section contains int8 input values after batch interleave\n";

  int dpuCfgPrllIch = 16;
  int dpuCfgDummy = 0;
  int layerCfgIbMode = 8;

  int src_b = BATCH_SIZE;
  
  int src_h = xmodel_->getInH();
  if(xmodel_->getChannelAugmentationMode())
    src_h = chAugH_;
  
  int src_w = xmodel_->getInW();
  if(xmodel_->getChannelAugmentationMode())
    src_w = chAugW_;
  
  int src_c = xmodel_->getInCh();
  if(xmodel_->getChannelAugmentationMode())
    src_c = chAugCh_;

  int prllIch = dpuCfgPrllIch;
  int dummyMode = dpuCfgDummy;
  int dst_c = 0;
  int dst_c_grp = 0;
  int ddr_size = 0;
  int dst_c_idx = 0;
  int8_t data = 0;

  if(src_c % prllIch == 0)
    dst_c = src_c;
  else
    dst_c = (std::floor(src_c/prllIch)+1)*prllIch;

  dst_c_grp = std::floor(dst_c/prllIch);
  ddr_size = src_b*dst_c*src_w*src_h;
  for(int h_idx=0; h_idx<src_h; h_idx++)
  {
    for(int w_idx=0; w_idx<src_w; w_idx++)
    {
      for(int ch_grp_idx=0; ch_grp_idx<dst_c_grp; ch_grp_idx++)
      {
        for(int bch_idx=0; bch_idx<src_b; bch_idx++)
        {
          if(layerCfgIbMode==8)
          {
            for(int ch_idx=0; ch_idx<prllIch; ch_idx++)
            {
              dst_c_idx = (ch_grp_idx*prllIch)+ch_idx;
              if(dst_c_idx<src_c)
              {
                //data = img_data_src[bch_idx][h_idx][w_idx][dst_c_idx] & 0xff
                if(xmodel_->getChannelAugmentationMode())
                  data = debugChAug_[(bch_idx*chAugH_*chAugW_*chAugCh_)+(h_idx*chAugW_*chAugCh_)+(w_idx*chAugCh_)+(dst_c_idx)]&0xff;
               else
                  data = debugInput_[(bch_idx*xmodel_->getInH()*xmodel_->getInW()*xmodel_->getInCh())+(h_idx*xmodel_->getInW()*xmodel_->getInCh())+(w_idx*xmodel_->getInCh())+(dst_c_idx)]&0xff;
              }
              else
              {
                if(dummyMode==0)
                  data=0;
                else if(dummyMode==1)
                  data=0xff;
                else
                  data=std::rand()%(256);///randomInt(0,255);/////
              }
              debugBatchInterleaved_.push_back(data);////
            }
          }
          else
          {
            for(int ch_idx=0; ch_idx<prllIch; ch_idx=ch_idx+4)
            {
              data=0;
              for(int b2_ch_idx=0; b2_ch_idx<4; b2_ch_idx++)
              {
                dst_c_idx = (ch_grp_idx*prllIch)+ch_idx+b2_ch_idx;
                if(dst_c_idx<src_c)
                {
                  //data |= (img_data_src[bch_idx][h_idx][w_idx][dst_c_idx] & 0x03) << (b2_ch_idx * 2)
                  if(xmodel_->getChannelAugmentationMode())
                    data |= (debugChAug_[(bch_idx*chAugH_*chAugW_*chAugCh_)+(h_idx*chAugW_*chAugCh_)+(w_idx*chAugCh_)+(dst_c_idx)] & 0x03) << (b2_ch_idx*2); /////;
                  else
                    data |= (debugInput_[(bch_idx*xmodel_->getInH()*xmodel_->getInW()*xmodel_->getInCh())+(h_idx*xmodel_->getInW()*xmodel_->getInCh())+(w_idx*xmodel_->getInCh())+(dst_c_idx)] & 0x03) << (b2_ch_idx*2); /////;
                }
                else
                {
                  if(dummyMode==0)
                    data |= 0;
                  else if(dummyMode==1)
                    data = (0x03 << (b2_ch_idx*2));
                  else
                    data=(std::rand()%(4))<<(b2_ch_idx*2);///randomInt;/////
                }
              }
              debugBatchInterleaved_.push_back(data);//////
            }
          }
        }
      }
    }
  }
  
  for(int i=0; i<debugBatchInterleaved_.size(); i++)
  {
    debug_dumpvals_<<"idx: "<<i<<" int8 val: "<<(int32_t)debugBatchInterleaved_[i]<<"\n";
  }
  debug_dumpvals_.close();
  
  convert2DmemFormat(debugBatchInterleaved_, "inputBatchInterleaved", xmodel_->getDebugDumpdir()+"inputBatchInterleaveDmem.txt");

}

void Dpuv3Int8DebugController::channelAug()
{
  
//  debugChAug_.resize(4*224*112*32, 0);
  std::cout<<"Executing Reshape on Host..."<<std::endl; 
  int src_kw = xmodel_->getInKernelW();
  int src_pw = xmodel_->getPadLft();
  int src_sw = xmodel_->getInStrdW();
  int src_b = BATCH_SIZE;
  int src_h = xmodel_->getInH();
  int src_w = xmodel_->getInW();
  int src_c = xmodel_->getInCh();
  
  int dst_b = src_b;
  int dst_h = src_h;
  int dst_w = std::floor(src_w/src_sw);
  int dst_c = src_c*src_kw;
  
  chAugW_ = dst_w;
  chAugH_ = dst_h;
  chAugCh_ = dst_c;

  int src_w_idx = 0;
  int src_c_idx = 0;
   

  debugChAug_.resize(dst_b*dst_h*dst_w*dst_c, 0);
  
  for(int bch_idx=0; bch_idx<dst_b; bch_idx++)
  {
    for(int h_idx=0; h_idx<dst_h; h_idx++)
    {
      for(int w_idx=0; w_idx<dst_w; w_idx++)
      {
        for(int ch_idx=0; ch_idx<dst_c; ch_idx++)
        {
          src_w_idx = (src_sw*w_idx)-src_pw+(std::floor(ch_idx/src_c));
          src_c_idx = ch_idx%src_c;
          
          if(src_w_idx<0 or src_w_idx>=src_w)
          {
//            img_data_dst[bch_idx][h_idx][w_idx][ch_idx] = 0
            debugChAug_[(bch_idx*dst_h*dst_w*dst_c)+(h_idx*dst_w*dst_c)+(w_idx*dst_c)+(ch_idx)]=0;
          }
          else
          {
  //          img_data_dst[bch_idx][h_idx][w_idx][ch_idx] = img_data_src[bch_idx][h_idx][src_w_idx][src_c_idx]
            debugChAug_[(bch_idx*dst_h*dst_w*dst_c)+(h_idx*dst_w*dst_c)+(w_idx*dst_c)+(ch_idx)] = debugInput_[(bch_idx*src_h*src_w*src_c)+(h_idx*src_w*src_c)+(src_w_idx*src_c)+(src_c_idx)];
          }

        }
      }
    }
  }


}


void Dpuv3Int8DebugController::debugDumpRegVals(uint64_t* buf_addr, uint32_t* reg_val)
{
    debug_dumpvals_.open(xmodel_->getDebugDumpdir()+"regVals.txt");
    debug_dumpvals_<<"*****************************************************\n";
    debug_dumpvals_<<"This section contains register values that are programmed onto kernel\n";
    
    debug_dumpvals_<<"CONTROL_ADDR_BLOCK_INSTR, reg: 0x"<<std::hex<<CONTROL_ADDR_BLOCK_INSTR<<" val: 0x"<<((buf_addr[BUF_IDX_INSTR]) & 0xFFFFFFFF)<<"\n";
    debug_dumpvals_<<"CONTROL_ADDR_BLOCK_INSTR + 0x4, reg: 0x"<<CONTROL_ADDR_BLOCK_INSTR + 0x4<<" val: 0x"<<(((buf_addr[BUF_IDX_INSTR]  ) >> 32) & 0xFFFFFFFF)<<"\n";
    
    if(not xmodel_->getSinglePoolDebug())
    {
      debug_dumpvals_<<"CONTROL_ADDR_BLOCK_PARAMS, reg: 0x"<<CONTROL_ADDR_BLOCK_PARAMS<<" val: 0x"<<((buf_addr[BUF_IDX_PARAMS] ) & 0xFFFFFFFF)<<"\n";
      debug_dumpvals_<<"CONTROL_ADDR_BLOCK_PARAMS + 0x4, reg: 0x"<<CONTROL_ADDR_BLOCK_PARAMS + 0x4<<" val: 0x"<<(((buf_addr[BUF_IDX_PARAMS] ) >> 32) & 0xFFFFFFFF)<<"\n";
    }
    
    debug_dumpvals_<<"CONTROL_ADDR_BLOCK_SWAP, reg: 0x"<<CONTROL_ADDR_BLOCK_SWAP<<" val: 0x"<<(( buf_addr[BUF_IDX_SWAP]   ) & 0xFFFFFFFF)<<"\n";
    debug_dumpvals_<<"CONTROL_ADDR_BLOCK_SWAP + 0x4, reg: 0x"<<CONTROL_ADDR_BLOCK_SWAP + 0x4<<" val: 0x"<<(((buf_addr[BUF_IDX_SWAP]   ) >> 32) & 0xFFFFFFFF)<<"\n";
    debug_dumpvals_<<"CONTROL_ADDR_BLOCK_RSLT, reg: 0x"<<CONTROL_ADDR_BLOCK_RSLT<<" val: 0x"<<(( buf_addr[BUF_IDX_RESULT] ) & 0xFFFFFFFF)<<"\n";
    debug_dumpvals_<<"CONTROL_ADDR_BLOCK_RSLT + 0x4, reg: 0x"<<CONTROL_ADDR_BLOCK_RSLT + 0x4<<" val: 0x"<<(((buf_addr[BUF_IDX_RESULT] ) >> 32) & 0xFFFFFFFF)<<"\n";
    debug_dumpvals_<<"CONTROL_ADDR_BLOCK_SRC, reg: 0x"<<CONTROL_ADDR_BLOCK_SRC<<" val: 0x"<<(( buf_addr[BUF_IDX_SRC]    ) & 0xFFFFFFFF)<<"\n";
    debug_dumpvals_<<"CONTROL_ADDR_BLOCK_SRC + 0x4, reg: 0x "<<CONTROL_ADDR_BLOCK_SRC + 0x4<<" val: 0x"<<(((buf_addr[BUF_IDX_SRC]    ) >> 32) & 0xFFFFFFFF)<<"\n";
    debug_dumpvals_<<"CONTROL_ADDR_BLOCK_DST, reg: 0x"<<CONTROL_ADDR_BLOCK_DST<<" val: 0x"<<(( buf_addr[BUF_IDX_DST]    ) & 0xFFFFFFFF)<<"\n";
    debug_dumpvals_<<"CONTROL_ADDR_BLOCK_DST + 0x4, reg: 0x"<<CONTROL_ADDR_BLOCK_DST + 0x4<<" val: 0x"<<(((buf_addr[BUF_IDX_DST]    ) >> 32) & 0xFFFFFFFF)<<"\n";
    debug_dumpvals_<<"CONTROL_ADDR_TASK_DRU_ADDR_STRD, reg: 0x"<<CONTROL_ADDR_TASK_DRU_ADDR_STRD<<" val: 0x"<<reg_val[REG_IDX_TASK_DRU_ADDR_STRD] <<"\n";
    debug_dumpvals_<<"CONTROL_ADDR_TASK_DRU_KW, reg: 0x"<<CONTROL_ADDR_TASK_DRU_KW<<" val: 0x"<<reg_val[REG_IDX_TASK_DRU_KW]<<"\n";
    debug_dumpvals_<<"CONTROL_ADDR_TASK_DRU_SW, reg: 0x"<<CONTROL_ADDR_TASK_DRU_SW<<" val: 0x"<<reg_val[REG_IDX_TASK_DRU_SW]<<"\n";
    debug_dumpvals_<<"CONTROL_ADDR_TASK_DRU_IC, reg: 0x"<<CONTROL_ADDR_TASK_DRU_IC<<" val: 0x"<<reg_val[REG_IDX_TASK_DRU_IC]<<"\n";
    debug_dumpvals_<<"CONTROL_ADDR_TASK_DRU_OW, reg: 0x"<<CONTROL_ADDR_TASK_DRU_OW<<" val: 0x"<<reg_val[REG_IDX_TASK_DRU_OW]<<"\n";
    debug_dumpvals_<<"CONTROL_ADDR_TASK_DRU_OH, reg: 0x"<<CONTROL_ADDR_TASK_DRU_OH<<" val: 0x"<<reg_val[REG_IDX_TASK_DRU_OH]<<"\n";
    debug_dumpvals_<<"CONTROL_ADDR_TASK_DRU_SRC_NTRANS, reg: 0x"<<CONTROL_ADDR_TASK_DRU_SRC_NTRANS<<" val: 0x"<<reg_val[REG_IDX_TASK_DRU_SRC_NTRANS]<<"\n";
    debug_dumpvals_<<"CONTROL_ADDR_TASK_DRU_DST_NTRANS, reg: 0x"<<CONTROL_ADDR_TASK_DRU_DST_NTRANS<<" val: 0x"<<reg_val[REG_IDX_TASK_DRU_DST_NTRANS]<<"\n";
    debug_dumpvals_<<"CONTROL_ADDR_TASK_DRU_PL_CORR, reg: 0x"<<CONTROL_ADDR_TASK_DRU_PL_CORR<<" val: 0x"<<reg_val[REG_IDX_TASK_DRU_PL_CORR]<<"\n";
    debug_dumpvals_<<"CONTROL_ADDR_TASK_DRU_PR_CORR, reg: 0x"<<CONTROL_ADDR_TASK_DRU_PR_CORR<<" val: 0x"<<reg_val[REG_IDX_TASK_DRU_PR_CORR]<<"\n";
    debug_dumpvals_<<"CONTROL_ADDR_TASK_DRU_IW_CORR, reg: 0x"<<CONTROL_ADDR_TASK_DRU_IW_CORR<<" val: 0x"<<reg_val[REG_IDX_TASK_DRU_IW_CORR]<<"\n";
    debug_dumpvals_<<"CONTROL_ADDR_TASK_DRU_SW_CORR, reg: 0x"<<CONTROL_ADDR_TASK_DRU_SW_CORR<<" val: 0x"<<reg_val[REG_IDX_TASK_DRU_SW_CORR]<<"\n";
    debug_dumpvals_<<"CONTROL_ADDR_TASK_DRU_WCG_CORR, reg: 0x"<<CONTROL_ADDR_TASK_DRU_WCG_CORR<<" val: 0x"<<reg_val[REG_IDX_TASK_DRU_WCG_CORR]<<"\n";
    debug_dumpvals_<<"CONTROL_ADDR_TASK_DRU_READ_MODE, reg: 0x"<<CONTROL_ADDR_TASK_DRU_READ_MODE<<" val: 0x"<<reg_val[REG_IDX_TASK_DRU_READ_MODE]<<"\n";
    debug_dumpvals_<<"CONTROL_ADDR_TASK_MODE, reg: 0x"<<CONTROL_ADDR_TASK_MODE<<" val: 0x"<<reg_val[REG_IDX_TASK_MODE]<<"\n";
    debug_dumpvals_<<"CONTROL_ADDR_REG_AXCACHE_AXOS, reg: 0x"<<CONTROL_ADDR_REG_AXCACHE_AXOS<<" val: 0x"<<reg_val[REG_IDX_REG_AXCACHE_AXOS]<<"\n";
    debug_dumpvals_<<"CONTROL_ADDR_REG_DPU_PROF_ENABLE, reg: 0x"<<CONTROL_ADDR_REG_DPU_PROF_ENABLE<<" val: 0x"<<reg_val[REG_IDX_REG_DPU_PROF_ENABLE]<<"\n";
    
    debug_dumpvals_<<"*****************************************************\n";
    debug_dumpvals_.close();

}

void Dpuv3Int8DebugController::debugDumpOutputs(void *std_data, void *result_data, int result_size)
{
      debug_dumpvals_.open(xmodel_->getDebugDumpdir()+"outBoard.txt");
      debug_dumpvals_<<"*****************************************************\n";
      debug_dumpvals_<<"This section contains int8 output values straight from board, not yet output reorganized\n";
      std::vector<int8_t> outBoard(result_size,0);
      for(int j=0; j<result_size; j++)
      {
        debug_dumpvals_<<std::dec<<"idx: "<<j<<" int8 val: "<<(int32_t)*(int8_t *)((long long)result_data+j)<<"\n";
        outBoard[j] = *(int8_t *)((long long)result_data+j);
      }
      convert2DmemFormat(outBoard, "outputs straight from board, not converted to std format", xmodel_->getDebugDumpdir()+"outBoardDmem.txt");
      debug_dumpvals_.close();

      debug_dumpvals_.open(xmodel_->getDebugDumpdir()+"outStdFormat.txt");
      debug_dumpvals_<<"*****************************************************\n";
      debug_dumpvals_<<"This section contains int8 output values reorganized to standard format, NHWC\n";
      std::vector<int8_t> outputStdFormat(result_size,0);
      for(int j=0; j<result_size; j++)
      {
        debug_dumpvals_<<"idx: "<<j<<" int8 val: "<<(int32_t)*(int8_t *)((long long)std_data+j)<<"\n";
        outputStdFormat[j]=*(int8_t *)((long long)std_data+j);
      }
      convert2DmemFormat(outputStdFormat, "outputs converted to std NHWC format", xmodel_->getDebugDumpdir()+"outStdDmem.txt");
      debug_dumpvals_.close();

}

