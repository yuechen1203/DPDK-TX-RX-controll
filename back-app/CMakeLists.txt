cmake_minimum_required(VERSION 3.16)
project(dpdk_tx_back_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MARIADB REQUIRED libmariadb)

option(ENABLE_DPDK "Build the real DPDK TX engine" OFF)

add_executable(dpdk-tx-control
    src/main.cpp
    src/HttpServer.cpp
    src/RuntimeState.cpp
    src/StreamHistoryStore.cpp
    src/TxEngine.cpp
    src/Utils.cpp
)

target_include_directories(dpdk-tx-control PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${MARIADB_INCLUDE_DIRS}
)

target_link_libraries(dpdk-tx-control PRIVATE
    OpenSSL::Crypto
    Threads::Threads
    ${MARIADB_LIBRARIES}
)

target_link_directories(dpdk-tx-control PRIVATE
    ${MARIADB_LIBRARY_DIRS}
)

if(ENABLE_DPDK)
    set(DPDKLIBS
        -Wl,--no-as-needed

        rte_common_cpt rte_common_iavf rte_bus_auxiliary rte_bus_pci rte_bus_vdev
        rte_bus_vmbus rte_common_cnxk rte_common_qat rte_mempool_bucket rte_mempool_cnxk
        rte_mempool_ring rte_mempool_stack rte_dma_cnxk rte_dma_skeleton
        rte_net_af_packet rte_net_bond rte_net_cnxk rte_net_failsafe rte_net_iavf rte_net_memif
        rte_net_null rte_net_ring rte_raw_cnxk_bphy rte_raw_skeleton rte_crypto_cnxk rte_crypto_null
        rte_crypto_scheduler rte_event_cnxk rte_event_dsw rte_event_opdl rte_event_skeleton
        rte_event_sw rte_acl
        -Wl,--as-needed

        rte_graph rte_pipeline rte_table rte_pdump rte_port rte_fib
        rte_ipsec rte_vhost rte_stack rte_security rte_sched rte_reorder rte_rib rte_dmadev
        rte_regexdev rte_rawdev rte_power rte_pcapng rte_member rte_lpm rte_latencystats
        rte_jobstats rte_ip_frag rte_gso rte_gro rte_gpudev rte_eventdev
        rte_efd rte_distributor rte_cryptodev rte_compressdev rte_cfgfile rte_bpf
        rte_bitratestats rte_bbdev rte_timer rte_hash rte_metrics rte_cmdline
        rte_pci rte_ethdev rte_meter rte_net rte_mbuf rte_mempool rte_rcu rte_ring rte_eal
        rte_telemetry rte_kvargs
    )
    target_compile_definitions(dpdk-tx-control PRIVATE
        DPTX_ENABLE_DPDK=1
        RTE_FORCE_INTRINSICS=1
    )
    target_include_directories(dpdk-tx-control PRIVATE /usr/local/dpdk/include)
    target_link_directories(dpdk-tx-control PRIVATE /usr/local/dpdk/lib)
    target_link_libraries(dpdk-tx-control PRIVATE ${DPDKLIBS})
endif()
