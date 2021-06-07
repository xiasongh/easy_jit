sudo apt-get update
sudo apt-get install -y software-properties-common
sudo apt-add-repository -y "ppa:ubuntu-toolchain-r/test" 

sudo apt-get install wget curl -y

sudo apt-get install -y ninja-build g++-11 libopencv-dev
sudo apt-get install -y build-essential git unzip cmake 

wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
./llvm.sh 11

sudo add-apt-repository universe
sudo apt install python2 -y
curl https://bootstrap.pypa.io/pip/2.7/get-pip.py --output get-pip.py
python2 get-pip.py 

pip install --user --upgrade "pip < 21.0"
pip install --user -Iv lit==0.7.0

mkdir _build && cd _build && git clone --depth=1 https://github.com/google/benchmark.git && git clone --depth=1 https://github.com/google/googletest.git benchmark/googletest

mkdir benchmark/_build && cd benchmark/_build && cmake .. -GNinja -DCMAKE_INSTALL_PREFIX=`pwd`/../_install && ninja && ninja install 
cd ../../

cmake -DLLVM_DIR=/usr/lib/llvm-11/cmake -DCMAKE_CXX_COMPILER=clang++-11 -DCMAKE_C_COMPILER=clang-11 -DEASY_JIT_EXAMPLE=ON -DEASY_JIT_BENCHMARK=ON -DBENCHMARK_DIR=`pwd`/benchmark/_install -DCMAKE_INSTALL_PREFIX=`pwd`/../_install .. -G Ninja
# RUN cd easy-just-in-time/_build && ninja && ninja install && ninja check && echo ok!
