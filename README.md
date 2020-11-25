Minimal & fast async runtime engine for Vitis accelerators. Capable of servicing > 800K requests per second.

### Overview

```
runner/src
  dpu_runner.cpp                 Implementation of Vitis API DpuRunner
                                 Initialize DpuController from meta.json or XIR
    execute_async()             
      lambda_func:               Lambda function submitted to engine Q, get job_id
        run()                    Call DpuController.run()
    wait()                       Wait for engine to complete job_id

device/src
  device_handle.cpp              Acquire FPGA DeviceHandle, store metadata
  device_memory.cpp              DeviceBuffer manages FPGA memory for TensorBuffer

controller/src
  dpu_controller.cpp             XRT programming for IP, holds one DeviceHandle
    DpuController()              Init IP core with meta.json or XIR
    run()
      alloc()                    Allocate in_addr/out_addr FPGA bufs for host ptrs
      upload()                   Write from host to FPGA DDR
      execute()                  Pass in_addr/out_addr to core, execute
      download()                 Read from FPGA to host DDR

engine/src
  engine.cpp                  
    Engine
      submit()                   Push new DPU task (lambda function) to Q
      wait()                     Block until task done

    EngineThreadPool
      run()                      Fetch new task from Q, exec user lambda_func

tests/
  engine/
    main.cpp                     Engine max throughput tests
    single_thread.cpp            shows >300K requests per second
    multi_thread.cpp             shows >800K requests per second

  app/
    main.cpp                     Vitis API app throughput tests, uses DpuRunner
    single_thread.cpp            shows ~4K requests per second (limited by DDR)
    multi_thread.cpp             shows ~13K requests per second (limited by DDR)
 
    models/
      sample_resnet50/
        meta.json                Describes configs/files to create this DpuRunner
```

### Requirements

* Xilinx Butler (http://xcdl190260/vitis/MLsuite/tree/master/packages, http://xcdl190260/vitis/XIP)
* XRT (https://github.com/Xilinx/XRT)
* json-c (https://github.com/json-c/json-c)

### Example

```
runner/src/dpu_runner.cpp:

std::pair<uint32_t, int> DpuRunner::execute_async(
  const std::vector<vart::TensorBuffer*>& inputs,
  const std::vector<vart::TensorBuffer*>& outputs) {
  Engine& engine = Engine::get_instance();
  auto job_id = engine.submit([this, &inputs, &outputs] {
    dpu_controller_[0]->run(inputs, outputs);
  });
  return std::pair<uint32_t, int>(job_id, 0);
}

int DpuRunner::wait(int jobid, int timeout) {
  Engine& engine = Engine::get_instance();
  engine.wait(jobid);

  return 0;
}
```

### Build
Makefile Method:  
```
make clean; make -j
```
CMake Method:  
```
mkdir build_release
cd build_release
cmake -DCMAKE_INSTALL_PREFIX=$CONDA_PREFIX -DCMAKE_PREFIX_PATH=$CONDA_PREFIX -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --parallel $CPU_COUNT
cmake --install .
```

### Run
Note the environment variables only necessary if you don't use Cmake Method
```
export LD_LIBRARY_PATH=build:${CONDA_PREFIX}/lib:/opt/xilinx/xrt/lib 
export XILINX_XRT=/opt/xilinx/xrt
build/tests/engine.exe
build/tests/app.exe -r tests/app/models/sample_resnet50/meta.json
```

### Conda environment setup (optional-recommended, to get all project dependencies)

```
wget https://repo.anaconda.com/archive/Anaconda2-5.1.0-Linux-x86_64.sh
bash ./Anaconda2-5.1.0-Linux-x86_64.sh
conda create -n rt-engine python=3.6 protobuf=3.11.2 json-c jsoncpp glog unilog target-factory xir vart -c defaults -c omnia -c conda-forge/label/gcc7 -c conda-forge
conda activate rt-engine
git clone gits@xcdl190260:vitis/XIP.git
cd XIP/Butler/src
(edit Makefile to skip ENABLE_PYBIND11 and -lpython if not needed)
make clean; make -j libbutler.so
mkdir -p ${CONDA_PREFIX}/include/xip/butler
cp client/butler_*.h ${CONDA_PREFIX}/include/xip/butler
cp common/*h ${CONDA_PREFIX}/include/xip/butler
cp butler.pb.h ${CONDA_PREFIX}/include/xip/butler
cp lib/libbutler.so ${CONDA_PREFIX}/lib/

```
