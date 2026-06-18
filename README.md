#Tx/Rx引擎

cd back-app

cmake -S . -B build-dpdk -DENABLE_DPDK=ON

cmake --build build-dpdk -j

./build-dpdk/dpdk-tx-control -f config/default.yaml

#配置参数见 ./config/default.yaml

#dpdk配置环境 头文件，库文件分别放置 /usr/local/dpdk/include , /usr/local/dpdk/lib





#前端

cd front-app-v2

npm i

npm run dev

#端口是5173
