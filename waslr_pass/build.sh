rm -rf build
mkdir build
cd build
cmake .. -DLLVM_ROOT=/usr
make
