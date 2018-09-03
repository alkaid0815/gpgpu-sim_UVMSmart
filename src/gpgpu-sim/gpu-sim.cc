// Copyright (c) 2009-2011, Tor M. Aamodt, Wilson W.L. Fung, George L. Yuan,
// Ali Bakhoda, Andrew Turner, Ivan Sham
// The University of British Columbia
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
// Redistributions in binary form must reproduce the above copyright notice, this
// list of conditions and the following disclaimer in the documentation and/or
// other materials provided with the distribution.
// Neither the name of The University of British Columbia nor the names of its
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


#include "gpu-sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <signal.h>
#include "zlib.h"


#include "shader.h"
#include "shader_trace.h"
#include "dram.h"
#include "mem_fetch.h"

#include <time.h>
#include "gpu-cache.h"
#include "gpu-misc.h"
#include "delayqueue.h"
#include "shader.h"
#include "icnt_wrapper.h"
#include "dram.h"
#include "addrdec.h"
#include "stat-tool.h"
#include "l2cache.h"

#include "../cuda-sim/ptx-stats.h"
#include "../statwrapper.h"
#include "../abstract_hardware_model.h"
#include "../debug.h"
#include "../gpgpusim_entrypoint.h"
#include "../cuda-sim/cuda-sim.h"
#include "../trace.h"
#include "mem_latency_stat.h"
#include "power_stat.h"
#include "visualizer.h"
#include "stats.h"
#include "../cuda-sim/cuda_device_runtime.h"

#ifdef GPGPUSIM_POWER_MODEL
#include "power_interface.h"
#else
class  gpgpu_sim_wrapper {};
#endif

#include <stdio.h>
#include <string.h>
#include <iostream>
#include <sstream>
#include <string>

#define MAX(a,b) (((a)>(b))?(a):(b))


bool g_interactive_debugger_enabled=false;

unsigned long long  gpu_sim_cycle = 0;
unsigned long long  gpu_tot_sim_cycle = 0;


// performance counter for stalls due to congestion.
unsigned int gpu_stall_dramfull = 0; 
unsigned int gpu_stall_icnt2sh = 0;
unsigned long long partiton_reqs_in_parallel = 0;
unsigned long long partiton_reqs_in_parallel_total = 0;
unsigned long long partiton_reqs_in_parallel_util = 0;
unsigned long long partiton_reqs_in_parallel_util_total = 0;
unsigned long long  gpu_sim_cycle_parition_util = 0;
unsigned long long  gpu_tot_sim_cycle_parition_util = 0;
unsigned long long partiton_replys_in_parallel = 0;
unsigned long long partiton_replys_in_parallel_total = 0;

/* Clock Domains */

#define  CORE  0x01
#define  L2    0x02
#define  DRAM  0x04
#define  ICNT  0x08  
#define  GMMU  0x10

#define MEM_LATENCY_STAT_IMPL




#include "mem_latency_stat.h"

void power_config::reg_options(class OptionParser * opp)
{


	  option_parser_register(opp, "-gpuwattch_xml_file", OPT_CSTR,
			  	  	  	  	 &g_power_config_name,"GPUWattch XML file",
	                   "gpuwattch.xml");

	   option_parser_register(opp, "-power_simulation_enabled", OPT_BOOL,
	                          &g_power_simulation_enabled, "Turn on power simulator (1=On, 0=Off)",
	                          "0");

	   option_parser_register(opp, "-power_per_cycle_dump", OPT_BOOL,
	                          &g_power_per_cycle_dump, "Dump detailed power output each cycle",
	                          "0");

	   // Output Data Formats
	   option_parser_register(opp, "-power_trace_enabled", OPT_BOOL,
	                          &g_power_trace_enabled, "produce a file for the power trace (1=On, 0=Off)",
	                          "0");

	   option_parser_register(opp, "-power_trace_zlevel", OPT_INT32,
	                          &g_power_trace_zlevel, "Compression level of the power trace output log (0=no comp, 9=highest)",
	                          "6");

	   option_parser_register(opp, "-steady_power_levels_enabled", OPT_BOOL,
	                          &g_steady_power_levels_enabled, "produce a file for the steady power levels (1=On, 0=Off)",
	                          "0");

	   option_parser_register(opp, "-steady_state_definition", OPT_CSTR,
			   	  &gpu_steady_state_definition, "allowed deviation:number of samples",
	                 	  "8:4");

}

void memory_config::reg_options(class OptionParser * opp)
{
    option_parser_register(opp, "-gpgpu_dram_scheduler", OPT_INT32, &scheduler_type, 
                                "0 = fifo, 1 = FR-FCFS (defaul)", "1");
    option_parser_register(opp, "-gpgpu_dram_partition_queues", OPT_CSTR, &gpgpu_L2_queue_config, 
                           "i2$:$2d:d2$:$2i",
                           "8:8:8:8");

    option_parser_register(opp, "-l2_ideal", OPT_BOOL, &l2_ideal, 
                           "Use a ideal L2 cache that always hit",
                           "0");
    option_parser_register(opp, "-gpgpu_cache:dl2", OPT_CSTR, &m_L2_config.m_config_string, 
                   "unified banked L2 data cache config "
                   " {<nsets>:<bsize>:<assoc>,<rep>:<wr>:<alloc>:<wr_alloc>,<mshr>:<N>:<merge>,<mq>}",
                   "64:128:8,L:B:m:N,A:16:4,4");
    option_parser_register(opp, "-gpgpu_cache:dl2_texture_only", OPT_BOOL, &m_L2_texure_only, 
                           "L2 cache used for texture only",
                           "1");
    option_parser_register(opp, "-gpgpu_n_mem", OPT_UINT32, &m_n_mem, 
                 "number of memory modules (e.g. memory controllers) in gpu",
                 "8");
    option_parser_register(opp, "-gpgpu_n_sub_partition_per_mchannel", OPT_UINT32, &m_n_sub_partition_per_memory_channel, 
                 "number of memory subpartition in each memory module",
                 "1");
    option_parser_register(opp, "-gpgpu_n_mem_per_ctrlr", OPT_UINT32, &gpu_n_mem_per_ctrlr, 
                 "number of memory chips per memory controller",
                 "1");
    option_parser_register(opp, "-gpgpu_memlatency_stat", OPT_INT32, &gpgpu_memlatency_stat, 
                "track and display latency statistics 0x2 enables MC, 0x4 enables queue logs",
                "0");
    option_parser_register(opp, "-gpgpu_frfcfs_dram_sched_queue_size", OPT_INT32, &gpgpu_frfcfs_dram_sched_queue_size, 
                "0 = unlimited (default); # entries per chip",
                "0");
    option_parser_register(opp, "-gpgpu_dram_return_queue_size", OPT_INT32, &gpgpu_dram_return_queue_size, 
                "0 = unlimited (default); # entries per chip",
                "0");
    option_parser_register(opp, "-gpgpu_dram_buswidth", OPT_UINT32, &busW, 
                 "default = 4 bytes (8 bytes per cycle at DDR)",
                 "4");
    option_parser_register(opp, "-gpgpu_dram_burst_length", OPT_UINT32, &BL, 
                 "Burst length of each DRAM request (default = 4 data bus cycle)",
                 "4");
    option_parser_register(opp, "-dram_data_command_freq_ratio", OPT_UINT32, &data_command_freq_ratio, 
                 "Frequency ratio between DRAM data bus and command bus (default = 2 times, i.e. DDR)",
                 "2");
    option_parser_register(opp, "-gpgpu_dram_timing_opt", OPT_CSTR, &gpgpu_dram_timing_opt, 
                "DRAM timing parameters = {nbk:tCCD:tRRD:tRCD:tRAS:tRP:tRC:CL:WL:tCDLR:tWR:nbkgrp:tCCDL:tRTPL}",
                "4:2:8:12:21:13:34:9:4:5:13:1:0:0");
    option_parser_register(opp, "-rop_latency", OPT_UINT32, &rop_latency,
                     "ROP queue latency (default 85)",
                     "85");
    option_parser_register(opp, "-dram_latency", OPT_UINT32, &dram_latency,
                     "DRAM latency (default 30)",
                     "30");

    m_address_mapping.addrdec_setoption(opp);
}

void shader_core_config::reg_options(class OptionParser * opp)
{
    option_parser_register(opp, "-gpgpu_simd_model", OPT_INT32, &model, 
                   "1 = post-dominator", "1");
    option_parser_register(opp, "-gpgpu_shader_core_pipeline", OPT_CSTR, &gpgpu_shader_core_pipeline_opt, 
                   "shader core pipeline config, i.e., {<nthread>:<warpsize>}",
                   "1024:32");
    option_parser_register(opp, "-gpgpu_tex_cache:l1", OPT_CSTR, &m_L1T_config.m_config_string, 
                   "per-shader L1 texture cache  (READ-ONLY) config "
                   " {<nsets>:<bsize>:<assoc>,<rep>:<wr>:<alloc>:<wr_alloc>,<mshr>:<N>:<merge>,<mq>:<rf>}",
                   "8:128:5,L:R:m:N,F:128:4,128:2");
    option_parser_register(opp, "-gpgpu_const_cache:l1", OPT_CSTR, &m_L1C_config.m_config_string, 
                   "per-shader L1 constant memory cache  (READ-ONLY) config "
                   " {<nsets>:<bsize>:<assoc>,<rep>:<wr>:<alloc>:<wr_alloc>,<mshr>:<N>:<merge>,<mq>} ",
                   "64:64:2,L:R:f:N,A:2:32,4" );
    option_parser_register(opp, "-gpgpu_cache:il1", OPT_CSTR, &m_L1I_config.m_config_string, 
                   "shader L1 instruction cache config "
                   " {<nsets>:<bsize>:<assoc>,<rep>:<wr>:<alloc>:<wr_alloc>,<mshr>:<N>:<merge>,<mq>} ",
                   "4:256:4,L:R:f:N,A:2:32,4" );
    option_parser_register(opp, "-gpgpu_cache:dl1", OPT_CSTR, &m_L1D_config.m_config_string,
                   "per-shader L1 data cache config "
                   " {<nsets>:<bsize>:<assoc>,<rep>:<wr>:<alloc>:<wr_alloc>,<mshr>:<N>:<merge>,<mq> | none}",
                   "none" );
    option_parser_register(opp, "-gpgpu_cache:dl1PrefL1", OPT_CSTR, &m_L1D_config.m_config_stringPrefL1,
                   "per-shader L1 data cache config "
                   " {<nsets>:<bsize>:<assoc>,<rep>:<wr>:<alloc>:<wr_alloc>,<mshr>:<N>:<merge>,<mq> | none}",
                   "none" );
    option_parser_register(opp, "-gpgpu_cache:dl1PrefShared", OPT_CSTR, &m_L1D_config.m_config_stringPrefShared,
                   "per-shader L1 data cache config "
                   " {<nsets>:<bsize>:<assoc>,<rep>:<wr>:<alloc>:<wr_alloc>,<mshr>:<N>:<merge>,<mq> | none}",
                   "none" );
    option_parser_register(opp, "-gmem_skip_L1D", OPT_BOOL, &gmem_skip_L1D, 
                   "global memory access skip L1D cache (implements -Xptxas -dlcm=cg, default=no skip)",
                   "0");

    option_parser_register(opp, "-gpgpu_perfect_mem", OPT_BOOL, &gpgpu_perfect_mem, 
                 "enable perfect memory mode (no cache miss)",
                 "0");
    option_parser_register(opp, "-n_regfile_gating_group", OPT_UINT32, &n_regfile_gating_group,
                 "group of lanes that should be read/written together)",
                 "4");
    option_parser_register(opp, "-gpgpu_clock_gated_reg_file", OPT_BOOL, &gpgpu_clock_gated_reg_file,
                 "enable clock gated reg file for power calculations",
                 "0");
    option_parser_register(opp, "-gpgpu_clock_gated_lanes", OPT_BOOL, &gpgpu_clock_gated_lanes,
                 "enable clock gated lanes for power calculations",
                 "0");
    option_parser_register(opp, "-gpgpu_shader_registers", OPT_UINT32, &gpgpu_shader_registers, 
                 "Number of registers per shader core. Limits number of concurrent CTAs. (default 8192)",
                 "8192");
    option_parser_register(opp, "-gpgpu_shader_cta", OPT_UINT32, &max_cta_per_core, 
                 "Maximum number of concurrent CTAs in shader (default 8)",
                 "8");
    option_parser_register(opp, "-gpgpu_num_cta_barriers", OPT_UINT32, &max_barriers_per_cta,
                 "Maximum number of named barriers per CTA (default 16)",
                 "16");
    option_parser_register(opp, "-gpgpu_n_clusters", OPT_UINT32, &n_simt_clusters, 
                 "number of processing clusters",
                 "10");
    option_parser_register(opp, "-gpgpu_n_cores_per_cluster", OPT_UINT32, &n_simt_cores_per_cluster, 
                 "number of simd cores per cluster",
                 "3");
    option_parser_register(opp, "-gpgpu_n_cluster_ejection_buffer_size", OPT_UINT32, &n_simt_ejection_buffer_size, 
                 "number of packets in ejection buffer",
                 "8");
    option_parser_register(opp, "-gpgpu_n_ldst_response_buffer_size", OPT_UINT32, &ldst_unit_response_queue_size, 
                 "number of response packets in ld/st unit ejection buffer",
                 "2");
    option_parser_register(opp, "-gpgpu_shmem_size", OPT_UINT32, &gpgpu_shmem_size,
                 "Size of shared memory per shader core (default 16kB)",
                 "16384");
    option_parser_register(opp, "-gpgpu_shmem_size", OPT_UINT32, &gpgpu_shmem_sizeDefault,
                 "Size of shared memory per shader core (default 16kB)",
                 "16384");
    option_parser_register(opp, "-gpgpu_shmem_size_PrefL1", OPT_UINT32, &gpgpu_shmem_sizePrefL1,
                 "Size of shared memory per shader core (default 16kB)",
                 "16384");
    option_parser_register(opp, "-gpgpu_shmem_size_PrefShared", OPT_UINT32, &gpgpu_shmem_sizePrefShared,
                 "Size of shared memory per shader core (default 16kB)",
                 "16384");
    option_parser_register(opp, "-gpgpu_shmem_num_banks", OPT_UINT32, &num_shmem_bank, 
                 "Number of banks in the shared memory in each shader core (default 16)",
                 "16");
    option_parser_register(opp, "-gpgpu_shmem_limited_broadcast", OPT_BOOL, &shmem_limited_broadcast, 
                 "Limit shared memory to do one broadcast per cycle (default on)",
                 "1");
    option_parser_register(opp, "-gpgpu_shmem_warp_parts", OPT_INT32, &mem_warp_parts,  
                 "Number of portions a warp is divided into for shared memory bank conflict check ",
                 "2");
    option_parser_register(opp, "-gpgpu_warpdistro_shader", OPT_INT32, &gpgpu_warpdistro_shader, 
                "Specify which shader core to collect the warp size distribution from", 
                "-1");
    option_parser_register(opp, "-gpgpu_warp_issue_shader", OPT_INT32, &gpgpu_warp_issue_shader, 
                "Specify which shader core to collect the warp issue distribution from", 
                "0");
    option_parser_register(opp, "-gpgpu_local_mem_map", OPT_BOOL, &gpgpu_local_mem_map, 
                "Mapping from local memory space address to simulated GPU physical address space (default = enabled)", 
                "1");
    option_parser_register(opp, "-gpgpu_num_reg_banks", OPT_INT32, &gpgpu_num_reg_banks, 
                "Number of register banks (default = 8)", 
                "8");
    option_parser_register(opp, "-gpgpu_reg_bank_use_warp_id", OPT_BOOL, &gpgpu_reg_bank_use_warp_id,
             "Use warp ID in mapping registers to banks (default = off)",
             "0");
    option_parser_register(opp, "-gpgpu_operand_collector_num_units_sp", OPT_INT32, &gpgpu_operand_collector_num_units_sp,
                "number of collector units (default = 4)", 
                "4");
    option_parser_register(opp, "-gpgpu_operand_collector_num_units_sfu", OPT_INT32, &gpgpu_operand_collector_num_units_sfu,
                "number of collector units (default = 4)", 
                "4");
    option_parser_register(opp, "-gpgpu_operand_collector_num_units_mem", OPT_INT32, &gpgpu_operand_collector_num_units_mem,
                "number of collector units (default = 2)", 
                "2");
    option_parser_register(opp, "-gpgpu_operand_collector_num_units_gen", OPT_INT32, &gpgpu_operand_collector_num_units_gen,
                "number of collector units (default = 0)", 
                "0");
    option_parser_register(opp, "-gpgpu_operand_collector_num_in_ports_sp", OPT_INT32, &gpgpu_operand_collector_num_in_ports_sp,
                           "number of collector unit in ports (default = 1)", 
                           "1");
    option_parser_register(opp, "-gpgpu_operand_collector_num_in_ports_sfu", OPT_INT32, &gpgpu_operand_collector_num_in_ports_sfu,
                           "number of collector unit in ports (default = 1)", 
                           "1");
    option_parser_register(opp, "-gpgpu_operand_collector_num_in_ports_mem", OPT_INT32, &gpgpu_operand_collector_num_in_ports_mem,
                           "number of collector unit in ports (default = 1)", 
                           "1");
    option_parser_register(opp, "-gpgpu_operand_collector_num_in_ports_gen", OPT_INT32, &gpgpu_operand_collector_num_in_ports_gen,
                           "number of collector unit in ports (default = 0)", 
                           "0");
    option_parser_register(opp, "-gpgpu_operand_collector_num_out_ports_sp", OPT_INT32, &gpgpu_operand_collector_num_out_ports_sp,
                           "number of collector unit in ports (default = 1)", 
                           "1");
    option_parser_register(opp, "-gpgpu_operand_collector_num_out_ports_sfu", OPT_INT32, &gpgpu_operand_collector_num_out_ports_sfu,
                           "number of collector unit in ports (default = 1)", 
                           "1");
    option_parser_register(opp, "-gpgpu_operand_collector_num_out_ports_mem", OPT_INT32, &gpgpu_operand_collector_num_out_ports_mem,
                           "number of collector unit in ports (default = 1)", 
                           "1");
    option_parser_register(opp, "-gpgpu_operand_collector_num_out_ports_gen", OPT_INT32, &gpgpu_operand_collector_num_out_ports_gen,
                           "number of collector unit in ports (default = 0)", 
                           "0");
    option_parser_register(opp, "-gpgpu_coalesce_arch", OPT_INT32, &gpgpu_coalesce_arch, 
                            "Coalescing arch (default = 13, anything else is off for now)", 
                            "13");
    option_parser_register(opp, "-gpgpu_num_sched_per_core", OPT_INT32, &gpgpu_num_sched_per_core, 
                            "Number of warp schedulers per core", 
                            "1");
    option_parser_register(opp, "-gpgpu_max_insn_issue_per_warp", OPT_INT32, &gpgpu_max_insn_issue_per_warp,
                            "Max number of instructions that can be issued per warp in one cycle by scheduler",
                            "2");
    option_parser_register(opp, "-gpgpu_simt_core_sim_order", OPT_INT32, &simt_core_sim_order,
                            "Select the simulation order of cores in a cluster (0=Fix, 1=Round-Robin)",
                            "1");
    option_parser_register(opp, "-gpgpu_pipeline_widths", OPT_CSTR, &pipeline_widths_string,
                            "Pipeline widths "
                            "ID_OC_SP,ID_OC_SFU,ID_OC_MEM,OC_EX_SP,OC_EX_SFU,OC_EX_MEM,EX_WB",
                            "1,1,1,1,1,1,1" );
    option_parser_register(opp, "-gpgpu_num_sp_units", OPT_INT32, &gpgpu_num_sp_units,
                            "Number of SP units (default=1)",
                            "1");
    option_parser_register(opp, "-gpgpu_num_sfu_units", OPT_INT32, &gpgpu_num_sfu_units,
                            "Number of SF units (default=1)",
                            "1");
    option_parser_register(opp, "-gpgpu_num_mem_units", OPT_INT32, &gpgpu_num_mem_units,
                            "Number if ldst units (default=1) WARNING: not hooked up to anything",
                             "1");
    option_parser_register(opp, "-gpgpu_scheduler", OPT_CSTR, &gpgpu_scheduler_string,
                                "Scheduler configuration: < lrr | gto | two_level_active > "
                                "If two_level_active:<num_active_warps>:<inner_prioritization>:<outer_prioritization>"
                                "For complete list of prioritization values see shader.h enum scheduler_prioritization_type"
                                "Default: gto",
                                 "gto");

    option_parser_register(opp, "-gpgpu_concurrent_kernel_sm", OPT_BOOL, &gpgpu_concurrent_kernel_sm, 
                "Support concurrent kernels on a SM (default = disabled)", 
                "0");

    option_parser_register(opp, "-tlb_size", OPT_INT32, &tlb_size,           
                            "Number of tlb entries per SM.",
                            "4096");
}

void gpgpu_sim_config::reg_options(option_parser_t opp)
{
    gpgpu_functional_sim_config::reg_options(opp);
    m_shader_config.reg_options(opp);
    m_memory_config.reg_options(opp);
    power_config::reg_options(opp);
   option_parser_register(opp, "-gpgpu_max_cycle", OPT_INT32, &gpu_max_cycle_opt, 
               "terminates gpu simulation early (0 = no limit)",
               "0");
   option_parser_register(opp, "-gpgpu_max_insn", OPT_INT32, &gpu_max_insn_opt, 
               "terminates gpu simulation early (0 = no limit)",
               "0");
   option_parser_register(opp, "-gpgpu_max_cta", OPT_INT32, &gpu_max_cta_opt, 
               "terminates gpu simulation early (0 = no limit)",
               "0");
   option_parser_register(opp, "-gpgpu_runtime_stat", OPT_CSTR, &gpgpu_runtime_stat, 
                  "display runtime statistics such as dram utilization {<freq>:<flag>}",
                  "10000:0");
   option_parser_register(opp, "-liveness_message_freq", OPT_INT64, &liveness_message_freq, 
               "Minimum number of seconds between simulation liveness messages (0 = always print)",
               "1");
   option_parser_register(opp, "-gpgpu_flush_l1_cache", OPT_BOOL, &gpgpu_flush_l1_cache,
                "Flush L1 cache at the end of each kernel call",
                "0");
   option_parser_register(opp, "-gpgpu_flush_l2_cache", OPT_BOOL, &gpgpu_flush_l2_cache,
                   "Flush L2 cache at the end of each kernel call",
                   "0");

   option_parser_register(opp, "-gpgpu_deadlock_detect", OPT_BOOL, &gpu_deadlock_detect, 
                "Stop the simulation at deadlock (1=on (default), 0=off)", 
                "1");
   option_parser_register(opp, "-gpgpu_ptx_instruction_classification", OPT_INT32, 
               &gpgpu_ptx_instruction_classification, 
               "if enabled will classify ptx instruction types per kernel (Max 255 kernels now)", 
               "0");
   option_parser_register(opp, "-gpgpu_ptx_sim_mode", OPT_INT32, &g_ptx_sim_mode, 
               "Select between Performance (default) or Functional simulation (1)", 
               "0");
   option_parser_register(opp, "-gpgpu_clock_domains", OPT_CSTR, &gpgpu_clock_domains, 
                  "Clock Domain Frequencies in MhZ {<Core Clock>:<ICNT Clock>:<L2 Clock>:<DRAM Clock>}",
                  "500.0:2000.0:2000.0:2000.0");
   option_parser_register(opp, "-gpgpu_max_concurrent_kernel", OPT_INT32, &max_concurrent_kernel,
                          "maximum kernels that can run concurrently on GPU", "8" );
   option_parser_register(opp, "-gpgpu_cflog_interval", OPT_INT32, &gpgpu_cflog_interval, 
               "Interval between each snapshot in control flow logger", 
               "0");
   option_parser_register(opp, "-visualizer_enabled", OPT_BOOL,
                          &g_visualizer_enabled, "Turn on visualizer output (1=On, 0=Off)",
                          "1");
   option_parser_register(opp, "-visualizer_outputfile", OPT_CSTR, 
                          &g_visualizer_filename, "Specifies the output log file for visualizer",
                          NULL);
   option_parser_register(opp, "-visualizer_zlevel", OPT_INT32,
                          &g_visualizer_zlevel, "Compression level of the visualizer output log (0=no comp, 9=highest)",
                          "6");
    option_parser_register(opp, "-trace_enabled", OPT_BOOL, 
                          &Trace::enabled, "Turn on traces",
                          "0");
    option_parser_register(opp, "-trace_components", OPT_CSTR, 
                          &Trace::config_str, "comma seperated list of traces to enable. "
                          "Complete list found in trace_streams.tup. "
                          "Default none",
                          "none");
    option_parser_register(opp, "-trace_sampling_core", OPT_INT32, 
                          &Trace::sampling_core, "The core which is printed using CORE_DPRINTF. Default 0",
                          "0");
    option_parser_register(opp, "-trace_sampling_memory_partition", OPT_INT32, 
                          &Trace::sampling_memory_partition, "The memory partition which is printed using MEMPART_DPRINTF. Default -1 (i.e. all)",
                          "-1");

    option_parser_register(opp, "-gddr_size", OPT_CSTR, &gddr_size_string,
               "Size of GDDR in MB/GB.(GLOBAL_HEAP_START, GLOBAL_HEAP_START + gddr_size) would be used for unmanged memory, (GLOBAL_HEAP_START + gddr_size, GLOBAL_HEAP_START + gddr_size*2) would be used for managed memory. ",
               "1GB");

    option_parser_register(opp, "-page_table_walk_latency", OPT_INT64, &page_table_walk_latency,                                                                                          
               "Average page table walk latency (in core cycle).",
               "100");

    option_parser_register(opp, "-num_of_lanes", OPT_INT32, &pcie_num_lanes,
               "Number of PCI-e lanes",
               "16");

    option_parser_register(opp, "-eviction_policy", OPT_CSTR, &eviction_policy,                 
               "Memory eviction policy: lru or random.",
               "lru");

    option_parser_register(opp, "-percentage_of_free_page_buffer", OPT_FLOAT, &free_page_buffer_percentage,     
               "Percentage of free page buffer to trigger the page eviction.",
               "0.0");

    option_parser_register(opp, "-page_size", OPT_CSTR, &page_size_string,                                                                                                                              
               "GDDR page size, only 4KB/2MB avaliable.",
               "4KB");
 
    option_parser_register(opp, "-pcie_transfer_rate", OPT_CSTR, &pcie_transfer_rate_string,          
               "PCI-e transfer rate per lane, per direction, in GT/s.",
               "8.0GT/s");

   ptx_file_line_stats_options(opp);

    //Jin: kernel launch latency
    extern unsigned g_kernel_launch_latency;
    option_parser_register(opp, "-gpgpu_kernel_launch_latency", OPT_INT32, 
                          &g_kernel_launch_latency, "Kernel launch latency in cycles. Default: 0",
                          "0");
    extern bool g_cdp_enabled;
    option_parser_register(opp, "-gpgpu_cdp_enabled", OPT_BOOL, 
                          &g_cdp_enabled, "Turn on CDP",
                          "0");
}

void gpgpu_sim_config::convert_byte_string()
{
   gpgpu_functional_sim_config::convert_byte_string();
   if(strstr(pcie_transfer_rate_string, "GT/s")) {
       pcie_transfer_rate = strtof(pcie_transfer_rate_string, NULL);
   } else { 
       printf("-pcie_transfer_rate should be in GT/s\n");
       exit(1);
   }     
 
}
/////////////////////////////////////////////////////////////////////////////

void increment_x_then_y_then_z( dim3 &i, const dim3 &bound)
{
   i.x++;
   if ( i.x >= bound.x ) {
      i.x = 0;
      i.y++;
      if ( i.y >= bound.y ) {
         i.y = 0;
         if( i.z < bound.z ) 
            i.z++;
      }
   }
}

void gpgpu_sim::launch( kernel_info_t *kinfo )
{
   unsigned cta_size = kinfo->threads_per_cta();
   if ( cta_size > m_shader_config->n_thread_per_shader ) {
      printf("Execution error: Shader kernel CTA (block) size is too large for microarch config.\n");
      printf("                 CTA size (x*y*z) = %u, max supported = %u\n", cta_size, 
             m_shader_config->n_thread_per_shader );
      printf("                 => either change -gpgpu_shader argument in gpgpusim.config file or\n");
      printf("                 modify the CUDA source to decrease the kernel block size.\n");
      abort();
   }
   unsigned n=0;
   for(n=0; n < m_running_kernels.size(); n++ ) {
       if( (NULL==m_running_kernels[n]) || m_running_kernels[n]->done() ) {
           m_running_kernels[n] = kinfo;
           break;
       }
   }
   assert(n < m_running_kernels.size());
}

bool gpgpu_sim::can_start_kernel()
{
   for(unsigned n=0; n < m_running_kernels.size(); n++ ) {
       if( (NULL==m_running_kernels[n]) || m_running_kernels[n]->done() ) 
           return true;
   }
   return false;
}

bool gpgpu_sim::hit_max_cta_count() const {
   if (m_config.gpu_max_cta_opt != 0) {
      if( (gpu_tot_issued_cta + m_total_cta_launched) >= m_config.gpu_max_cta_opt )
          return true;
   }
   return false;
}

bool gpgpu_sim::kernel_more_cta_left(kernel_info_t *kernel) const {
    if(hit_max_cta_count())
       return false;

    if(kernel && !kernel->no_more_ctas_to_run())
        return true;

    return false;
}

bool gpgpu_sim::get_more_cta_left() const
{ 
   if(hit_max_cta_count())
      return false;

   for(unsigned n=0; n < m_running_kernels.size(); n++ ) {
       if( m_running_kernels[n] && !m_running_kernels[n]->no_more_ctas_to_run() ) 
           return true;
   }
   return false;
}

kernel_info_t *gpgpu_sim::select_kernel()
{
    if(m_running_kernels[m_last_issued_kernel] &&
        !m_running_kernels[m_last_issued_kernel]->no_more_ctas_to_run()) {
        unsigned launch_uid = m_running_kernels[m_last_issued_kernel]->get_uid(); 
        if(std::find(m_executed_kernel_uids.begin(), m_executed_kernel_uids.end(), launch_uid) == m_executed_kernel_uids.end()) {
            m_running_kernels[m_last_issued_kernel]->start_cycle = gpu_sim_cycle + gpu_tot_sim_cycle;
            m_executed_kernel_uids.push_back(launch_uid); 
            m_executed_kernel_names.push_back(m_running_kernels[m_last_issued_kernel]->name()); 
        }
        return m_running_kernels[m_last_issued_kernel];
    }

    for(unsigned n=0; n < m_running_kernels.size(); n++ ) {
        unsigned idx = (n+m_last_issued_kernel+1)%m_config.max_concurrent_kernel;
        if( kernel_more_cta_left(m_running_kernels[idx]) ){
            m_last_issued_kernel=idx;
            m_running_kernels[idx]->start_cycle = gpu_sim_cycle + gpu_tot_sim_cycle;
            // record this kernel for stat print if it is the first time this kernel is selected for execution  
            unsigned launch_uid = m_running_kernels[idx]->get_uid(); 
            assert(std::find(m_executed_kernel_uids.begin(), m_executed_kernel_uids.end(), launch_uid) == m_executed_kernel_uids.end());
            m_executed_kernel_uids.push_back(launch_uid); 
            m_executed_kernel_names.push_back(m_running_kernels[idx]->name()); 

            return m_running_kernels[idx];
        }
    }
    return NULL;
}

unsigned gpgpu_sim::finished_kernel()
{
    if( m_finished_kernel.empty() ) 
        return 0;
    unsigned result = m_finished_kernel.front();
    m_finished_kernel.pop_front();
    return result;
}

void gpgpu_sim::set_kernel_done( kernel_info_t *kernel ) 
{ 
    unsigned uid = kernel->get_uid();
    m_finished_kernel.push_back(uid);
    std::vector<kernel_info_t*>::iterator k;
    for( k=m_running_kernels.begin(); k!=m_running_kernels.end(); k++ ) {
        if( *k == kernel ) {
            kernel->end_cycle = gpu_sim_cycle + gpu_tot_sim_cycle;
            *k = NULL;
            break;
        }
    }
    assert( k != m_running_kernels.end() ); 
}

void gpgpu_sim::stop_all_running_kernels(){
    std::vector<kernel_info_t *>::iterator k;
    for(k = m_running_kernels.begin(); k != m_running_kernels.end(); ++k){
        if(*k != NULL){ // If a kernel is active
            set_kernel_done(*k); // Stop the kernel
            assert(*k==NULL);
        }
    }
}

void set_ptx_warp_size(const struct core_config * warp_size);

gpgpu_sim::gpgpu_sim( const gpgpu_sim_config &config ) 
    : gpgpu_t(config), m_config(config)
{ 
    m_shader_config = &m_config.m_shader_config;
    m_memory_config = &m_config.m_memory_config;
    set_ptx_warp_size(m_shader_config);
    ptx_file_line_stats_create_exposed_latency_tracker(m_config.num_shader());

#ifdef GPGPUSIM_POWER_MODEL
        m_gpgpusim_wrapper = new gpgpu_sim_wrapper(config.g_power_simulation_enabled,config.g_power_config_name);
#endif

    m_shader_stats = new shader_core_stats(m_shader_config);
    m_memory_stats = new memory_stats_t(m_config.num_shader(),m_shader_config,m_memory_config);
    average_pipeline_duty_cycle = (float *)malloc(sizeof(float));
    active_sms=(float *)malloc(sizeof(float));
    m_power_stats = new power_stat_t(m_shader_config,average_pipeline_duty_cycle,active_sms,m_shader_stats,m_memory_config,m_memory_stats);

    gpu_sim_insn = 0;
    gpu_tot_sim_insn = 0;
    gpu_tot_issued_cta = 0;
    m_total_cta_launched = 0;
    gpu_deadlock = false;

    m_gmmu = new gmmu_t(this, config );

    m_cluster = new simt_core_cluster*[m_shader_config->n_simt_clusters];
    for (unsigned i=0;i<m_shader_config->n_simt_clusters;i++) 
        m_cluster[i] = new simt_core_cluster(this,i,m_shader_config,m_memory_config,m_shader_stats,m_memory_stats);

    m_memory_partition_unit = new memory_partition_unit*[m_memory_config->m_n_mem];
    m_memory_sub_partition = new memory_sub_partition*[m_memory_config->m_n_mem_sub_partition];
    for (unsigned i=0;i<m_memory_config->m_n_mem;i++) {
        m_memory_partition_unit[i] = new memory_partition_unit(i, m_memory_config, m_memory_stats);
        for (unsigned p = 0; p < m_memory_config->m_n_sub_partition_per_memory_channel; p++) {
            unsigned submpid = i * m_memory_config->m_n_sub_partition_per_memory_channel + p; 
            m_memory_sub_partition[submpid] = m_memory_partition_unit[i]->get_sub_partition(p); 
        }
    }

    icnt_wrapper_init();
    icnt_create(m_shader_config->n_simt_clusters,m_memory_config->m_n_mem_sub_partition);

    time_vector_create(NUM_MEM_REQ_STAT);
    fprintf(stdout, "GPGPU-Sim uArch: performance model initialization complete.\n");

    m_running_kernels.resize( config.max_concurrent_kernel, NULL );
    m_last_issued_kernel = 0;
    m_last_cluster_issue = 0;
    *average_pipeline_duty_cycle=0;
    *active_sms=0;

    last_liveness_message_time = 0;
   
   //Jin: functional simulation for CDP
   m_functional_sim = false;
   m_functional_sim_kernel = NULL;
}

int gpgpu_sim::shared_mem_size() const
{
   return m_shader_config->gpgpu_shmem_size;
}

int gpgpu_sim::num_registers_per_core() const
{
   return m_shader_config->gpgpu_shader_registers;
}

int gpgpu_sim::wrp_size() const
{
   return m_shader_config->warp_size;
}

int gpgpu_sim::shader_clock() const
{
   return m_config.core_freq/1000;
}

void gpgpu_sim::set_prop( cudaDeviceProp *prop )
{
   m_cuda_properties = prop;
}

const struct cudaDeviceProp *gpgpu_sim::get_prop() const
{
   return m_cuda_properties;
}

enum divergence_support_t gpgpu_sim::simd_model() const
{
   return m_shader_config->model;
}

void gpgpu_sim_config::init_clock_domains(void ) 
{
   sscanf(gpgpu_clock_domains,"%lf:%lf:%lf:%lf", 
          &core_freq, &icnt_freq, &l2_freq, &dram_freq);
   core_freq = core_freq MhZ;
   icnt_freq = icnt_freq MhZ;
   l2_freq = l2_freq MhZ;
   dram_freq = dram_freq MhZ;        
   core_period = 1/core_freq;
   icnt_period = 1/icnt_freq;
   dram_period = 1/dram_freq;
   l2_period = 1/l2_freq;
   printf("GPGPU-Sim uArch: clock freqs: %lf:%lf:%lf:%lf\n",core_freq,icnt_freq,l2_freq,dram_freq);
   printf("GPGPU-Sim uArch: clock periods: %.20lf:%.20lf:%.20lf:%.20lf\n",core_period,icnt_period,l2_period,dram_period);
}

void gpgpu_sim::reinit_clock_domains(void)
{
   core_time = 0;
   dram_time = 0;
   icnt_time = 0;
   l2_time = 0;
   gmmu_time = 0;
}

bool gpgpu_sim::active()
{
    if (m_config.gpu_max_cycle_opt && (gpu_tot_sim_cycle + gpu_sim_cycle) >= m_config.gpu_max_cycle_opt) 
       return false;
    if (m_config.gpu_max_insn_opt && (gpu_tot_sim_insn + gpu_sim_insn) >= m_config.gpu_max_insn_opt) 
       return false;
    if (m_config.gpu_max_cta_opt && (gpu_tot_issued_cta >= m_config.gpu_max_cta_opt) )
       return false;
    if (m_config.gpu_deadlock_detect && gpu_deadlock) 
       return false;
    for (unsigned i=0;i<m_shader_config->n_simt_clusters;i++) 
       if( m_cluster[i]->get_not_completed()>0 ) 
           return true;;
    for (unsigned i=0;i<m_memory_config->m_n_mem;i++) 
       if( m_memory_partition_unit[i]->busy()>0 )
           return true;;
    if( icnt_busy() )
        return true;
    if( get_more_cta_left() )
        return true;
    return false;
}

void gpgpu_sim::init()
{
    // run a CUDA grid on the GPU microarchitecture simulator
    gpu_sim_cycle = 0;
    gpu_sim_insn = 0;
    last_gpu_sim_insn = 0;
    m_total_cta_launched=0;
    partiton_reqs_in_parallel = 0;
    partiton_replys_in_parallel = 0;
    partiton_reqs_in_parallel_util = 0;
    gpu_sim_cycle_parition_util = 0;

    reinit_clock_domains();
    set_param_gpgpu_num_shaders(m_config.num_shader());
    for (unsigned i=0;i<m_shader_config->n_simt_clusters;i++) 
       m_cluster[i]->reinit();
    m_shader_stats->new_grid();
    // initialize the control-flow, memory access, memory latency logger
    if (m_config.g_visualizer_enabled) {
        create_thread_CFlogger( m_config.num_shader(), m_shader_config->n_thread_per_shader, 0, m_config.gpgpu_cflog_interval );
    }
    shader_CTA_count_create( m_config.num_shader(), m_config.gpgpu_cflog_interval);
    if (m_config.gpgpu_cflog_interval != 0) {
       insn_warp_occ_create( m_config.num_shader(), m_shader_config->warp_size );
       shader_warp_occ_create( m_config.num_shader(), m_shader_config->warp_size, m_config.gpgpu_cflog_interval);
       shader_mem_acc_create( m_config.num_shader(), m_memory_config->m_n_mem, 4, m_config.gpgpu_cflog_interval);
       shader_mem_lat_create( m_config.num_shader(), m_config.gpgpu_cflog_interval);
       shader_cache_access_create( m_config.num_shader(), 3, m_config.gpgpu_cflog_interval);
       set_spill_interval (m_config.gpgpu_cflog_interval * 40);
    }

    if (g_network_mode)
       icnt_init();

    // McPAT initialization function. Called on first launch of GPU
#ifdef GPGPUSIM_POWER_MODEL
    if(m_config.g_power_simulation_enabled){
        init_mcpat(m_config, m_gpgpusim_wrapper, m_config.gpu_stat_sample_freq,  gpu_tot_sim_insn, gpu_sim_insn);
    }
#endif
}

void gpgpu_sim::update_stats() {
    m_memory_stats->memlatstat_lat_pw();
    gpu_tot_sim_cycle += gpu_sim_cycle;
    gpu_tot_sim_insn += gpu_sim_insn;
    gpu_tot_issued_cta += m_total_cta_launched;
    partiton_reqs_in_parallel_total += partiton_reqs_in_parallel;
    partiton_replys_in_parallel_total += partiton_replys_in_parallel;
    partiton_reqs_in_parallel_util_total += partiton_reqs_in_parallel_util;
    gpu_tot_sim_cycle_parition_util += gpu_sim_cycle_parition_util ;

    gpu_sim_cycle = 0;
    partiton_reqs_in_parallel = 0;
    partiton_replys_in_parallel = 0;
    partiton_reqs_in_parallel_util = 0;
    gpu_sim_cycle_parition_util = 0;
    gpu_sim_insn = 0;
    m_total_cta_launched = 0;
}

void gpgpu_sim::print_stats()
{
    ptx_file_line_stats_write_file();
    gpu_print_stat();

    if (g_network_mode) {
        printf("----------------------------Interconnect-DETAILS--------------------------------\n" );
        icnt_display_stats();
        icnt_display_overall_stats();
        printf("----------------------------END-of-Interconnect-DETAILS-------------------------\n" );
    }
}

void gpgpu_sim::deadlock_check()
{
   if (m_config.gpu_deadlock_detect && gpu_deadlock) {
      fflush(stdout);
      printf("\n\nGPGPU-Sim uArch: ERROR ** deadlock detected: last writeback core %u @ gpu_sim_cycle %u (+ gpu_tot_sim_cycle %u) (%u cycles ago)\n", 
             gpu_sim_insn_last_update_sid,
             (unsigned) gpu_sim_insn_last_update, (unsigned) (gpu_tot_sim_cycle-gpu_sim_cycle),
             (unsigned) (gpu_sim_cycle - gpu_sim_insn_last_update )); 
      unsigned num_cores=0;
      for (unsigned i=0;i<m_shader_config->n_simt_clusters;i++) {
         unsigned not_completed = m_cluster[i]->get_not_completed();
         if( not_completed ) {
             if ( !num_cores )  {
                 printf("GPGPU-Sim uArch: DEADLOCK  shader cores no longer committing instructions [core(# threads)]:\n" );
                 printf("GPGPU-Sim uArch: DEADLOCK  ");
                 m_cluster[i]->print_not_completed(stdout);
             } else if (num_cores < 8 ) {
                 m_cluster[i]->print_not_completed(stdout);
             } else if (num_cores >= 8 ) {
                 printf(" + others ... ");
             }
             num_cores+=m_shader_config->n_simt_cores_per_cluster;
         }
      }
      printf("\n");
      for (unsigned i=0;i<m_memory_config->m_n_mem;i++) {
         bool busy = m_memory_partition_unit[i]->busy();
         if( busy ) 
             printf("GPGPU-Sim uArch DEADLOCK:  memory partition %u busy\n", i );
      }
      if( icnt_busy() ) {
         printf("GPGPU-Sim uArch DEADLOCK:  iterconnect contains traffic\n");
         icnt_display_state( stdout );
      }
      printf("\nRe-run the simulator in gdb and use debug routines in .gdbinit to debug this\n");
      fflush(stdout);
      abort();
   }
}

/// printing the names and uids of a set of executed kernels (usually there is only one)
std::string gpgpu_sim::executed_kernel_info_string() 
{
   std::stringstream statout; 

   statout << "kernel_name = "; 
   for (unsigned int k = 0; k < m_executed_kernel_names.size(); k++) {
      statout << m_executed_kernel_names[k] << " "; 
   }
   statout << std::endl; 
   statout << "kernel_launch_uid = ";
   for (unsigned int k = 0; k < m_executed_kernel_uids.size(); k++) {
      statout << m_executed_kernel_uids[k] << " "; 
   }
   statout << std::endl; 

   return statout.str(); 
}
void gpgpu_sim::set_cache_config(std::string kernel_name,  FuncCache cacheConfig )
{
	m_special_cache_config[kernel_name]=cacheConfig ;
}

FuncCache gpgpu_sim::get_cache_config(std::string kernel_name)
{
	for (	std::map<std::string, FuncCache>::iterator iter = m_special_cache_config.begin(); iter != m_special_cache_config.end(); iter++){
		    std::string kernel= iter->first;
			if (kernel_name.compare(kernel) == 0){
				return iter->second;
			}
	}
	return (FuncCache)0;
}

bool gpgpu_sim::has_special_cache_config(std::string kernel_name)
{
	for (	std::map<std::string, FuncCache>::iterator iter = m_special_cache_config.begin(); iter != m_special_cache_config.end(); iter++){
	    	std::string kernel= iter->first;
			if (kernel_name.compare(kernel) == 0){
				return true;
			}
	}
	return false;
}


void gpgpu_sim::set_cache_config(std::string kernel_name)
{
	if(has_special_cache_config(kernel_name)){
		change_cache_config(get_cache_config(kernel_name));
	}else{
		change_cache_config(FuncCachePreferNone);
	}
}


void gpgpu_sim::change_cache_config(FuncCache cache_config)
{
	if(cache_config != m_shader_config->m_L1D_config.get_cache_status()){
		printf("FLUSH L1 Cache at configuration change between kernels\n");
		for (unsigned i=0;i<m_shader_config->n_simt_clusters;i++) {
			m_cluster[i]->cache_flush();
	    }
	}

	switch(cache_config){
	case FuncCachePreferNone:
		m_shader_config->m_L1D_config.init(m_shader_config->m_L1D_config.m_config_string, FuncCachePreferNone);
		m_shader_config->gpgpu_shmem_size=m_shader_config->gpgpu_shmem_sizeDefault;
		break;
	case FuncCachePreferL1:
		if((m_shader_config->m_L1D_config.m_config_stringPrefL1 == NULL) || (m_shader_config->gpgpu_shmem_sizePrefL1 == (unsigned)-1))
		{
			printf("WARNING: missing Preferred L1 configuration\n");
			m_shader_config->m_L1D_config.init(m_shader_config->m_L1D_config.m_config_string, FuncCachePreferNone);
			m_shader_config->gpgpu_shmem_size=m_shader_config->gpgpu_shmem_sizeDefault;

		}else{
			m_shader_config->m_L1D_config.init(m_shader_config->m_L1D_config.m_config_stringPrefL1, FuncCachePreferL1);
			m_shader_config->gpgpu_shmem_size=m_shader_config->gpgpu_shmem_sizePrefL1;
		}
		break;
	case FuncCachePreferShared:
		if((m_shader_config->m_L1D_config.m_config_stringPrefShared == NULL) || (m_shader_config->gpgpu_shmem_sizePrefShared == (unsigned)-1))
		{
			printf("WARNING: missing Preferred L1 configuration\n");
			m_shader_config->m_L1D_config.init(m_shader_config->m_L1D_config.m_config_string, FuncCachePreferNone);
			m_shader_config->gpgpu_shmem_size=m_shader_config->gpgpu_shmem_sizeDefault;
		}else{
			m_shader_config->m_L1D_config.init(m_shader_config->m_L1D_config.m_config_stringPrefShared, FuncCachePreferShared);
			m_shader_config->gpgpu_shmem_size=m_shader_config->gpgpu_shmem_sizePrefShared;
		}
		break;
	default:
		break;
	}
}


void gpgpu_sim::clear_executed_kernel_info()
{
   m_executed_kernel_names.clear();
   m_executed_kernel_uids.clear();
}
void gpgpu_sim::gpu_print_stat() 
{  
   FILE *statfout = stdout; 

   std::string kernel_info_str = executed_kernel_info_string(); 
   fprintf(statfout, "%s", kernel_info_str.c_str()); 

   printf("gpu_sim_cycle = %lld\n", gpu_sim_cycle);
   printf("gpu_sim_insn = %lld\n", gpu_sim_insn);
   printf("gpu_ipc = %12.4f\n", (float)gpu_sim_insn / gpu_sim_cycle);
   printf("gpu_tot_sim_cycle = %lld\n", gpu_tot_sim_cycle+gpu_sim_cycle);
   printf("gpu_tot_sim_insn = %lld\n", gpu_tot_sim_insn+gpu_sim_insn);
   printf("gpu_tot_ipc = %12.4f\n", (float)(gpu_tot_sim_insn+gpu_sim_insn) / (gpu_tot_sim_cycle+gpu_sim_cycle));
   printf("gpu_tot_issued_cta = %lld\n", gpu_tot_issued_cta + m_total_cta_launched);

   extern unsigned long long g_max_total_param_size;
   fprintf(statfout, "max_total_param_size = %llu\n", g_max_total_param_size);

   // performance counter for stalls due to congestion.
   printf("gpu_stall_dramfull = %d\n", gpu_stall_dramfull);
   printf("gpu_stall_icnt2sh    = %d\n", gpu_stall_icnt2sh );

   printf("partiton_reqs_in_parallel = %lld\n", partiton_reqs_in_parallel);
   printf("partiton_reqs_in_parallel_total    = %lld\n", partiton_reqs_in_parallel_total );
   printf("partiton_level_parallism = %12.4f\n", (float)partiton_reqs_in_parallel / gpu_sim_cycle);
   printf("partiton_level_parallism_total  = %12.4f\n", (float)(partiton_reqs_in_parallel+partiton_reqs_in_parallel_total) / (gpu_tot_sim_cycle+gpu_sim_cycle) );
   printf("partiton_reqs_in_parallel_util = %lld\n", partiton_reqs_in_parallel_util);
   printf("partiton_reqs_in_parallel_util_total    = %lld\n", partiton_reqs_in_parallel_util_total );
   printf("gpu_sim_cycle_parition_util = %lld\n", gpu_sim_cycle_parition_util);
   printf("gpu_tot_sim_cycle_parition_util    = %lld\n", gpu_tot_sim_cycle_parition_util );
   printf("partiton_level_parallism_util = %12.4f\n", (float)partiton_reqs_in_parallel_util / gpu_sim_cycle_parition_util);
   printf("partiton_level_parallism_util_total  = %12.4f\n", (float)(partiton_reqs_in_parallel_util+partiton_reqs_in_parallel_util_total) / (gpu_sim_cycle_parition_util+gpu_tot_sim_cycle_parition_util) );
   printf("partiton_replys_in_parallel = %lld\n", partiton_replys_in_parallel);
   printf("partiton_replys_in_parallel_total    = %lld\n", partiton_replys_in_parallel_total );
   printf("L2_BW  = %12.4f GB/Sec\n", ((float)(partiton_replys_in_parallel * 32) / (gpu_sim_cycle * m_config.icnt_period)) / 1000000000);
   printf("L2_BW_total  = %12.4f GB/Sec\n", ((float)((partiton_replys_in_parallel+partiton_replys_in_parallel_total) * 32) / ((gpu_tot_sim_cycle+gpu_sim_cycle) * m_config.icnt_period)) / 1000000000 );

   time_t curr_time;
   time(&curr_time);
   unsigned long long elapsed_time = MAX( curr_time - g_simulation_starttime, 1 );
   printf( "gpu_total_sim_rate=%u\n", (unsigned)( ( gpu_tot_sim_insn + gpu_sim_insn ) / elapsed_time ) );

   //shader_print_l1_miss_stat( stdout );
   shader_print_cache_stats(stdout);

   cache_stats core_cache_stats;
   core_cache_stats.clear();
   for(unsigned i=0; i<m_config.num_cluster(); i++){
       m_cluster[i]->get_cache_stats(core_cache_stats);
   }
   printf("\nTotal_core_cache_stats:\n");
   core_cache_stats.print_stats(stdout, "Total_core_cache_stats_breakdown");
   shader_print_scheduler_stat( stdout, false );

   m_shader_stats->print(stdout);
#ifdef GPGPUSIM_POWER_MODEL
   if(m_config.g_power_simulation_enabled){
	   m_gpgpusim_wrapper->print_power_kernel_stats(gpu_sim_cycle, gpu_tot_sim_cycle, gpu_tot_sim_insn + gpu_sim_insn, kernel_info_str, true );
	   mcpat_reset_perf_count(m_gpgpusim_wrapper);
   }
#endif

   // performance counter that are not local to one shader
   m_memory_stats->memlatstat_print(m_memory_config->m_n_mem,m_memory_config->nbk);
   for (unsigned i=0;i<m_memory_config->m_n_mem;i++)
      m_memory_partition_unit[i]->print(stdout);

   // L2 cache stats
   if(!m_memory_config->m_L2_config.disabled()){
       cache_stats l2_stats;
       struct cache_sub_stats l2_css;
       struct cache_sub_stats total_l2_css;
       l2_stats.clear();
       l2_css.clear();
       total_l2_css.clear();

       printf("\n========= L2 cache stats =========\n");
       for (unsigned i=0;i<m_memory_config->m_n_mem_sub_partition;i++){
           m_memory_sub_partition[i]->accumulate_L2cache_stats(l2_stats);
           m_memory_sub_partition[i]->get_L2cache_sub_stats(l2_css);

           fprintf( stdout, "L2_cache_bank[%d]: Access = %u, Miss = %u, Miss_rate = %.3lf, Pending_hits = %u, Reservation_fails = %u\n",
                    i, l2_css.accesses, l2_css.misses, (double)l2_css.misses / (double)l2_css.accesses, l2_css.pending_hits, l2_css.res_fails);

           total_l2_css += l2_css;
       }
       if (!m_memory_config->m_L2_config.disabled() && m_memory_config->m_L2_config.get_num_lines()) {
          //L2c_print_cache_stat();
          printf("L2_total_cache_accesses = %u\n", total_l2_css.accesses);
          printf("L2_total_cache_misses = %u\n", total_l2_css.misses);
          if(total_l2_css.accesses > 0)
              printf("L2_total_cache_miss_rate = %.4lf\n", (double)total_l2_css.misses/(double)total_l2_css.accesses);
          printf("L2_total_cache_pending_hits = %u\n", total_l2_css.pending_hits);
          printf("L2_total_cache_reservation_fails = %u\n", total_l2_css.res_fails);
          printf("L2_total_cache_breakdown:\n");
          l2_stats.print_stats(stdout, "L2_cache_stats_breakdown");
          total_l2_css.print_port_stats(stdout, "L2_cache");
       }
   }

   if (m_config.gpgpu_cflog_interval != 0) {
      spill_log_to_file (stdout, 1, gpu_sim_cycle);
      insn_warp_occ_print(stdout);
   }
   if ( gpgpu_ptx_instruction_classification ) {
      StatDisp( g_inst_classification_stat[g_ptx_kernel_count]);
      StatDisp( g_inst_op_classification_stat[g_ptx_kernel_count]);
   }

#ifdef GPGPUSIM_POWER_MODEL
   if(m_config.g_power_simulation_enabled){
       m_gpgpusim_wrapper->detect_print_steady_state(1,gpu_tot_sim_insn+gpu_sim_insn);
   }
#endif


   // Interconnect power stat print
   long total_simt_to_mem=0;
   long total_mem_to_simt=0;
   long temp_stm=0;
   long temp_mts = 0;
   for(unsigned i=0; i<m_config.num_cluster(); i++){
	   m_cluster[i]->get_icnt_stats(temp_stm, temp_mts);
	   total_simt_to_mem += temp_stm;
	   total_mem_to_simt += temp_mts;
   }
   printf("\nicnt_total_pkts_mem_to_simt=%ld\n", total_mem_to_simt);
   printf("icnt_total_pkts_simt_to_mem=%ld\n", total_simt_to_mem);

   time_vector_print();
   fflush(stdout);

   clear_executed_kernel_info(); 
}


// performance counter that are not local to one shader
unsigned gpgpu_sim::threads_per_core() const 
{ 
   return m_shader_config->n_thread_per_shader; 
}

void shader_core_ctx::mem_instruction_stats(const warp_inst_t &inst)
{
    unsigned active_count = inst.active_count(); 
    //this breaks some encapsulation: the is_[space] functions, if you change those, change this.
    switch (inst.space.get_type()) {
    case undefined_space:
    case reg_space:
        break;
    case shared_space:
        m_stats->gpgpu_n_shmem_insn += active_count; 
        break;
    case const_space:
        m_stats->gpgpu_n_const_insn += active_count;
        break;
    case param_space_kernel:
    case param_space_local:
        m_stats->gpgpu_n_param_insn += active_count;
        break;
    case tex_space:
        m_stats->gpgpu_n_tex_insn += active_count;
        break;
    case global_space:
    case local_space:
        if( inst.is_store() )
            m_stats->gpgpu_n_store_insn += active_count;
        else 
            m_stats->gpgpu_n_load_insn += active_count;
        break;
    default:
        abort();
    }
}
bool shader_core_ctx::can_issue_1block(kernel_info_t & kernel) {

   //Jin: concurrent kernels on one SM
   if(m_config->gpgpu_concurrent_kernel_sm) {    
      if(m_config->max_cta(kernel) < 1)
           return false;

      return occupy_shader_resource_1block(kernel, false);
   }
   else {
      return (get_n_active_cta() < m_config->max_cta(kernel));
   } 
}

int shader_core_ctx::find_available_hwtid(unsigned int cta_size, bool occupy) {
   
   unsigned int step;
   for(step = 0; step < m_config->n_thread_per_shader; 
        step += cta_size) {

        unsigned int hw_tid;
        for(hw_tid = step; hw_tid < step + cta_size;
            hw_tid++) {
            if(m_occupied_hwtid.test(hw_tid))
                break;
        }
        if(hw_tid == step + cta_size) //consecutive non-active
            break;
   }
   if(step >= m_config->n_thread_per_shader) //didn't find
     return -1;
   else {
     if(occupy) {
        for(unsigned hw_tid = step; hw_tid < step + cta_size;
            hw_tid++)
            m_occupied_hwtid.set(hw_tid);
     }
     return step;
   }
}

bool shader_core_ctx::occupy_shader_resource_1block(kernel_info_t & k, bool occupy) {
   unsigned threads_per_cta  = k.threads_per_cta();
   const class function_info *kernel = k.entry();
   unsigned int padded_cta_size = threads_per_cta;
   unsigned int warp_size = m_config->warp_size; 
   if (padded_cta_size%warp_size) 
      padded_cta_size = ((padded_cta_size/warp_size)+1)*(warp_size);

   if(m_occupied_n_threads + padded_cta_size > m_config->n_thread_per_shader)
     return false;

   if(find_available_hwtid(padded_cta_size, false) == -1)
     return false;

   const struct gpgpu_ptx_sim_info *kernel_info = ptx_sim_kernel_info(kernel);

   if(m_occupied_shmem + kernel_info->smem > m_config->gpgpu_shmem_size)
     return false;

   unsigned int used_regs = padded_cta_size * ((kernel_info->regs+3)&~3);
   if(m_occupied_regs + used_regs > m_config->gpgpu_shader_registers)
     return false;

   if(m_occupied_ctas +1 > m_config->max_cta_per_core)
     return false;
   
   if(occupy) {
       m_occupied_n_threads += padded_cta_size;
       m_occupied_shmem += kernel_info->smem;
       m_occupied_regs += (padded_cta_size * ((kernel_info->regs+3)&~3));
       m_occupied_ctas++;

      SHADER_DPRINTF(LIVENESS, "GPGPU-Sim uArch: Occupied %d threads, %d shared mem, %d registers, %d ctas\n",
            m_occupied_n_threads, m_occupied_shmem, m_occupied_regs, m_occupied_ctas);  
   }

   return true;
}

void shader_core_ctx::release_shader_resource_1block(unsigned hw_ctaid, kernel_info_t & k) {

   if(m_config->gpgpu_concurrent_kernel_sm) {
      unsigned threads_per_cta  = k.threads_per_cta();
      const class function_info *kernel = k.entry();
      unsigned int padded_cta_size = threads_per_cta;
      unsigned int warp_size = m_config->warp_size; 
      if (padded_cta_size%warp_size) 
         padded_cta_size = ((padded_cta_size/warp_size)+1)*(warp_size);
   
      assert(m_occupied_n_threads >= padded_cta_size);
      m_occupied_n_threads -= padded_cta_size;
   
      int start_thread = m_occupied_cta_to_hwtid[hw_ctaid];
   
      for(unsigned hwtid = start_thread; hwtid < start_thread + padded_cta_size;
       hwtid++)
          m_occupied_hwtid.reset(hwtid);
      m_occupied_cta_to_hwtid.erase(hw_ctaid);
   
      const struct gpgpu_ptx_sim_info *kernel_info = ptx_sim_kernel_info(kernel);
   
      assert(m_occupied_shmem >= (unsigned int)kernel_info->smem);
      m_occupied_shmem -= kernel_info->smem;
   
      unsigned int used_regs = padded_cta_size * ((kernel_info->regs+3)&~3);
      assert(m_occupied_regs >= used_regs);
      m_occupied_regs -= used_regs;
   
      assert(m_occupied_ctas >= 1);
      m_occupied_ctas--;
   }
}

////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * Launches a cooperative thread array (CTA). 
 *  
 * @param kernel 
 *    object that tells us which kernel to ask for a CTA from 
 */

void shader_core_ctx::issue_block2core( kernel_info_t &kernel ) 
{

    if(!m_config->gpgpu_concurrent_kernel_sm)
        set_max_cta(kernel);
    else
        assert(occupy_shader_resource_1block(kernel, true));

    kernel.inc_running();

    // find a free CTA context 
    unsigned free_cta_hw_id=(unsigned)-1;

    unsigned max_cta_per_core;
    if(!m_config->gpgpu_concurrent_kernel_sm)
        max_cta_per_core = kernel_max_cta_per_shader;
    else
        max_cta_per_core = m_config->max_cta_per_core;
    for (unsigned i=0;i<max_cta_per_core;i++ ) {
      if( m_cta_status[i]==0 ) {
         free_cta_hw_id=i;
         break;
      }
    }
    assert( free_cta_hw_id!=(unsigned)-1 );

    // determine hardware threads and warps that will be used for this CTA
    int cta_size = kernel.threads_per_cta();

    // hw warp id = hw thread id mod warp size, so we need to find a range 
    // of hardware thread ids corresponding to an integral number of hardware
    // thread ids
    int padded_cta_size = cta_size; 
    if (cta_size%m_config->warp_size)
      padded_cta_size = ((cta_size/m_config->warp_size)+1)*(m_config->warp_size);

    unsigned int start_thread, end_thread;

    if(!m_config->gpgpu_concurrent_kernel_sm) {
        start_thread = free_cta_hw_id * padded_cta_size;
        end_thread  = start_thread +  cta_size;
    }
    else {
        start_thread = find_available_hwtid(padded_cta_size, true);
        assert((int)start_thread != -1);
        end_thread = start_thread + cta_size;
        assert(m_occupied_cta_to_hwtid.find(free_cta_hw_id) == m_occupied_cta_to_hwtid.end());
        m_occupied_cta_to_hwtid[free_cta_hw_id]= start_thread;
    }

    // reset the microarchitecture state of the selected hardware thread and warp contexts
    reinit(start_thread, end_thread,false);
     
    // initalize scalar threads and determine which hardware warps they are allocated to
    // bind functional simulation state of threads to hardware resources (simulation) 
    warp_set_t warps;
    unsigned nthreads_in_block= 0;
    for (unsigned i = start_thread; i<end_thread; i++) {
        m_threadState[i].m_cta_id = free_cta_hw_id;
        unsigned warp_id = i/m_config->warp_size;
        nthreads_in_block += ptx_sim_init_thread(kernel,&m_thread[i],m_sid,i,cta_size-(i-start_thread),m_config->n_thread_per_shader,this,free_cta_hw_id,warp_id,m_cluster->get_gpu());
        m_threadState[i].m_active = true; 
        warps.set( warp_id );
    }
    assert( nthreads_in_block > 0 && nthreads_in_block <= m_config->n_thread_per_shader); // should be at least one, but less than max
    m_cta_status[free_cta_hw_id]=nthreads_in_block;

    // now that we know which warps are used in this CTA, we can allocate
    // resources for use in CTA-wide barrier operations
    m_barriers.allocate_barrier(free_cta_hw_id,warps);

    // initialize the SIMT stacks and fetch hardware
    init_warps( free_cta_hw_id, start_thread, end_thread);
    m_n_active_cta++;

    shader_CTA_count_log(m_sid, 1);
    SHADER_DPRINTF(LIVENESS, "GPGPU-Sim uArch: cta:%2u, start_tid:%4u, end_tid:%4u, initialized @(%lld,%lld)\n", 
        free_cta_hw_id, start_thread, end_thread, gpu_sim_cycle, gpu_tot_sim_cycle );

}

///////////////////////////////////////////////////////////////////////////////////////////

void dram_t::dram_log( int task ) 
{
   if (task == SAMPLELOG) {
      StatAddSample(mrqq_Dist, que_length());   
   } else if (task == DUMPLOG) {
      printf ("Queue Length DRAM[%d] ",id);StatDisp(mrqq_Dist);
   }
}

//Find next clock domain and increment its time
int gpgpu_sim::next_clock_domain(void) 
{
   // to get the cycles spent for any cuda stream operation before and after the kernel is launched
   // monotonically increase the total simulation cycle
   if( !active() ) {
        int mask = 0x00;
        mask |= GMMU;
        gpu_tot_sim_cycle++;
        return mask;
   }

   double smallest = min4(core_time,icnt_time,dram_time,gmmu_time);
   int mask = 0x00;
   if ( l2_time <= smallest ) {
      smallest = l2_time;
      mask |= L2 ;
      l2_time += m_config.l2_period;
   }
   if ( icnt_time <= smallest ) {
      mask |= ICNT;
      icnt_time += m_config.icnt_period;
   }
   if ( dram_time <= smallest ) {
      mask |= DRAM;
      dram_time += m_config.dram_period;
   }
   if ( core_time <= smallest ) {
      mask |= CORE;
      core_time += m_config.core_period;
   }
   if ( gmmu_time <= smallest) {
      mask |= GMMU; 
      gmmu_time += m_config.core_period;
   }
   return mask;
}

void gpgpu_sim::issue_block2core()
{
    unsigned last_issued = m_last_cluster_issue; 
    for (unsigned i=0;i<m_shader_config->n_simt_clusters;i++) {
        unsigned idx = (i + last_issued + 1) % m_shader_config->n_simt_clusters;
        unsigned num = m_cluster[idx]->issue_block2core();
        if( num ) {
            m_last_cluster_issue=idx;
            m_total_cta_launched += num;
        }
    }
}

unsigned long long g_single_step=0; // set this in gdb to single step the pipeline


gmmu_t::gmmu_t(class gpgpu_sim* gpu, const gpgpu_sim_config &config)
	:m_gpu(gpu),m_config(config), pcie_latency( (unsigned long long) (8.0 * (float)m_config.page_size * m_config.core_freq / m_config.pcie_transfer_rate / 1000000000.0) )
{
    m_shader_config = &m_config.m_shader_config;
    if( std::string(m_config.eviction_policy) == "lru" ) {
       policy = eviction_policy::LRU; 
    } else if (	std::string(m_config.eviction_policy) == "random" ) {
       policy = eviction_policy::RANDOM;
    } else {
        printf("Unknown eviction policy"); 
        exit(1);
    }
}

void gmmu_t::accessed_pages_erase(mem_addr_t page_num)
{
    assert( find( accessed_pages.begin(), accessed_pages.end(), page_num ) != accessed_pages.end());
    accessed_pages.erase( find( accessed_pages.begin(), accessed_pages.end(), page_num ) );
}

void gmmu_t::register_tlbflush_callback(std::function<void(mem_addr_t)> cb_tlb)
{
    callback_tlb_flush.push_back(cb_tlb);
}

void gmmu_t::tlb_flush(mem_addr_t page_num)
{
    for ( list<std::function<void(mem_addr_t)> >::iterator iter = callback_tlb_flush.begin();
	 iter != callback_tlb_flush.end(); iter++) {
        (*iter)(page_num);
    }
}

void gmmu_t::check_write_stage_queue(mem_addr_t page_num)
{
    // the page, about to be accessed, was selected for eviction earlier 
    // so don't evict that page
    // choose another page instead to make balance of free list
    if ( std::find(pcie_write_stage_queue.begin(), pcie_write_stage_queue.end(), page_num ) != pcie_write_stage_queue.end() ) {
        pcie_write_stage_queue.erase( std::find( pcie_write_stage_queue.begin(), pcie_write_stage_queue.end(),  page_num ) ); 
	page_eviction_procedure();
    }    
}

void gmmu_t::page_eviction_procedure()
{
    assert( !accessed_pages.empty() );

    mem_addr_t page_num;

    // in lru, only evict the least recently used pages at the front of accessed pages queue
    if( policy == eviction_policy::LRU ) {
        page_num =  accessed_pages.front() ;
    } else {
    // in random eviction, select a random page
	list<mem_addr_t>::iterator iter = accessed_pages.begin();
        std::advance(iter, rand() % accessed_pages.size() );
        page_num = *iter;
    }


    if( m_gpu->get_global_memory()->is_page_dirty(page_num) ) {
        pcie_write_stage_queue.push_back( page_num );
    } else {
	// if the page is not touched, evict it directly
	m_gpu->get_global_memory()->invalidate_page( page_num );
        m_gpu->get_global_memory()->clear_page_access( page_num ); 

        m_gpu->get_global_memory()->free_pages(1);

	tlb_flush( page_num);
    }
   
    if( policy == eviction_policy::LRU ) {
        accessed_pages.pop_front();
    } else {
	accessed_pages.erase ( find( accessed_pages.begin(), accessed_pages.end(), page_num ) );
    }
}

void gmmu_t::page_refresh(mem_access_t ma)
{
    mem_addr_t page_num = m_gpu->get_global_memory()->get_page_num( ma.get_addr() );

    m_gpu->get_global_memory()->set_page_access(page_num);

    // on write (store) set the dirty flag
    if ( ma.get_type() == GLOBAL_ACC_W) {
	m_gpu->get_global_memory()->set_page_dirty(page_num);
    } 
   
    // push the page and the end of the accessed page list to implement LRU
    if( find( accessed_pages.begin(), accessed_pages.end(), page_num ) != accessed_pages.end() ){
	accessed_pages.erase ( find( accessed_pages.begin(), accessed_pages.end(), page_num ) );
    }

    accessed_pages.push_back(page_num);	
}

void gmmu_t::activate_prefetch(mem_addr_t m_device_addr, size_t m_cnt, struct CUstream_st *m_stream)
{
     for(std::list<prefetch_req>::iterator iter = prefetch_req_buffer.begin(); iter!=prefetch_req_buffer.end(); iter++){
	 if(iter->start_addr == m_device_addr && iter->size == m_cnt && iter->m_stream->get_uid() == m_stream->get_uid()) {
		assert(iter->cur_addr == m_device_addr);
		iter->active = true;
		return;
	 }
     }
}

void gmmu_t::register_prefetch(mem_addr_t m_device_addr, mem_addr_t m_device_allocation_ptr, size_t m_cnt, struct CUstream_st *m_stream)
{
    struct prefetch_req pre_q;

    pre_q.start_addr = m_device_addr;
    pre_q.cur_addr = m_device_addr;
    pre_q.allocation_addr = m_device_allocation_ptr;
    pre_q.size = m_cnt;
    pre_q.active = false;
    pre_q.m_stream = m_stream;

    prefetch_req_buffer.push_back(pre_q);
}

#define MAX_PREFETCH_SIZE 2*1024*1024

void gmmu_t::cycle()
{
    int simt_cluster_id = 0;
    
    // check the pcie write latency queue
    if ( !pcie_write_latency_queue.empty() ) {
                  
        std::list<pcie_latency_t>::iterator iter = pcie_write_latency_queue.begin();
                  
        // only if the first page finishes trasfer
        if ((gpu_sim_cycle+gpu_tot_sim_cycle) >= iter->ready_cycle) {
                    
            // pcie transfer for multiple lanes may finish transer 
            while ( iter != pcie_write_latency_queue.end() &&
                    ((gpu_sim_cycle + gpu_tot_sim_cycle) >= iter->ready_cycle) ) {

                m_gpu->gpu_writeback(m_gpu->get_global_memory()->get_mem_addr(pcie_write_latency_queue.front().page_num));

                pcie_write_latency_queue.pop_front();

                iter = pcie_write_latency_queue.begin();
            }    
        }    
    }    

    unsigned schedule = pcie_write_latency_queue.size();

    // schedule a write back transfer if there is a write back request in staging queue and a free lane
    while ( !pcie_write_stage_queue.empty() && schedule < m_config.pcie_num_lanes) {
        struct pcie_latency_t p_t; 

        p_t.page_num = pcie_write_stage_queue.front();
        p_t.ready_cycle = gpu_sim_cycle + gpu_tot_sim_cycle + pcie_latency;
        pcie_write_latency_queue.push_back(p_t);

	assert( m_gpu->get_global_memory()->is_page_dirty(pcie_write_stage_queue.front()) );

        m_gpu->get_global_memory()->invalidate_page( pcie_write_stage_queue.front() );
	m_gpu->get_global_memory()->clear_page_dirty( pcie_write_stage_queue.front() );
	m_gpu->get_global_memory()->clear_page_access( pcie_write_stage_queue.front() );

        m_gpu->get_global_memory()->free_pages(1);
        
	tlb_flush( pcie_write_stage_queue.front() );

        pcie_write_stage_queue.pop_front();

        schedule++;
    }    

    // check the pcie queue
    if ( !pcie_read_latency_queue.empty() ) {
             
        std::list<pcie_latency_t>::iterator iter = pcie_read_latency_queue.begin();
             
        // only if the first page finishes trasfer
        if ((gpu_sim_cycle+gpu_tot_sim_cycle) >= iter->ready_cycle) {
               
            // pcie transfer for multiple lanes may finish transer 
            while ( iter != pcie_read_latency_queue.end() &&
                    ((gpu_sim_cycle+gpu_tot_sim_cycle) >= iter->ready_cycle) ) {

                // validate the page in page table
                m_gpu->get_global_memory()->validate_page(iter->page_num);

		assert(req_info.find(iter->page_num) != req_info.end());

                // check if the transferred page is part of a prefetch request
		if ( !prefetch_req_buffer.empty() ) {

        	    prefetch_req& pre_q = prefetch_req_buffer.front();

		    std::list<mem_addr_t>::iterator iter2 = find(pre_q.pending_prefetch.begin(), pre_q.pending_prefetch.end(), iter->page_num);

		    if( iter2 != pre_q.pending_prefetch.end() ) {
			
                        // pending prefetch holds the list of 4KB pages of a big chunk of tranfer (max upto 2MB)
                        // remove it from the list as the PCI-e has transferred the page
			pre_q.pending_prefetch.erase(iter2);
		
		        // if this page is part of current prefecth request 
                        // add all the dependant memory requests to the outgoing_replayable_nacks
                        // these should be replayed only when current block of memory transfer is finished
			pre_q.outgoing_replayable_nacks[iter->page_num].merge(req_info[iter->page_num]);

                        // erase the page from the MSHR map
			req_info.erase(req_info.find(iter->page_num));			
		    }

		}

		// this page request is created by core on page fault and not part of a prefetch
		if( req_info.find(iter->page_num) != req_info.end()) {

                    // for all memory fetches that were waiting for this page, should be replayed back for cache access 
		    for ( std::list<mem_fetch*>::iterator iter2 = req_info[iter->page_num].begin();
                          iter2 != req_info[iter->page_num].end(); iter2++){
                          mem_fetch* mf = *iter2;
                        
                          simt_cluster_id = mf->get_sid() / m_config.num_core_per_cluster();

                          // push the memory fetch into the gmmu to cu queue
                          (m_gpu->getSIMTCluster(simt_cluster_id))->push_gmmu_cu_queue(mf);
                    }    
               
                    // erase the page from the MSHR map
                    req_info.erase(req_info.find(iter->page_num));

		}

                pcie_read_latency_queue.pop_front();

                iter = pcie_read_latency_queue.begin();
            } 
        } 
    }

    schedule = pcie_read_latency_queue.size();

    // schedule a transfer if there is a page request in staging queue and a free lane
    while ( !pcie_read_stage_queue.empty() && schedule < m_config.pcie_num_lanes && m_gpu->get_global_memory()->get_free_pages() > 0) {
        struct pcie_latency_t p_t;

        p_t.page_num = pcie_read_stage_queue.front();
        p_t.ready_cycle = gpu_sim_cycle + gpu_tot_sim_cycle + pcie_latency;
        pcie_read_latency_queue.push_back(p_t);

	pcie_read_stage_queue.pop_front();

	m_gpu->get_global_memory()->alloc_pages(1);
	
        schedule++;
    }

    // check the page_table_walk_delay_queue
    while ( !page_table_walk_queue.empty() &&
            ((gpu_sim_cycle+gpu_tot_sim_cycle) >= page_table_walk_queue.front().ready_cycle )) {

        mem_fetch* mf = page_table_walk_queue.front().mf;

        list<mem_addr_t> page_list = m_gpu->get_global_memory()->get_faulty_pages(mf->get_addr(), mf->get_access_size());

        // if there is no page fault, directly return to the upward queue of cluster
        if ( page_list.empty() ) {
	    mem_addr_t page_num = m_gpu->get_global_memory()->get_page_num( mf->get_mem_access().get_addr());
	    check_write_stage_queue(page_num);

            simt_cluster_id = mf->get_sid() / m_config.num_core_per_cluster();
            (m_gpu->getSIMTCluster(simt_cluster_id))->push_gmmu_cu_queue(mf);
        } else {
            assert(page_list.size() == 1); 
     
	    
	    // the page request is already there in MSHR either as a page fault or as part of scheduled prefetch request
	    if ( req_info.find( *(page_list.begin()) ) != req_info.end()) {

		 req_info[*(page_list.begin())].push_back(mf);
	    } else {

	    	 // if the memory fetch is part of any requests in the prefetch command buffer
                 // then add it to the incoming replayable_nacks
		 std::list<prefetch_req>::iterator iter;

	    	 for( iter = prefetch_req_buffer.begin(); iter != prefetch_req_buffer.end(); iter++) {

		      if( iter->start_addr <= mf->get_addr() &&
		          mf->get_addr() < iter->start_addr + iter->size) {
 
 			  iter->incoming_replayable_nacks[page_list.front()].push_back(mf);
			  break;
		      }
	    	 }

	         // if the memory fetch is not part of any request in the prefetch command buffer
	         if ( iter == prefetch_req_buffer.end()) {

                     // one memory fetch request should only have one page fault
                     // because they are always coalesced
 	             // if there is already a page in the staging queue, don't need to add it again
		     assert( req_info.find( *(page_list.begin()) ) == req_info.end() );

                     // for each page fault, the possible eviction procudure will only be called once here
                     // if the number of free pages (subtracted when popped from read stage and pushed into read latency) is smaller than 
                     // the number of staged read requests, then call the eviction
                     if ( m_gpu->get_global_memory()->should_evict_page(pcie_read_stage_queue.size(), m_config.free_page_buffer_percentage) ) {
                          page_eviction_procedure();
                     }    

                     pcie_read_stage_queue.push_back(page_list.front());
		     req_info[*(page_list.begin())].push_back(mf);
	         }
	    }
        }

        page_table_walk_queue.pop_front();
    }
    
    // fetch from cluster's cu to gmmu queue and push it into the page table way delay queue
    for (unsigned i=0; i<m_shader_config->n_simt_clusters; i++) {

        if(!(m_gpu->getSIMTCluster(i))->empty_cu_gmmu_queue()) {

            mem_fetch* mf = (m_gpu->getSIMTCluster(i))->front_cu_gmmu_queue();

            struct page_table_walk_latency_t pt_t;
            pt_t.mf = mf;
            pt_t.ready_cycle = gpu_sim_cycle+gpu_tot_sim_cycle + m_config.page_table_walk_latency;

            page_table_walk_queue.push_back(pt_t);    

            (m_gpu->getSIMTCluster(i))->pop_cu_gmmu_queue();
        }
    }

    // check if there is an active outstanding prefetch request
    if( !prefetch_req_buffer.empty() && prefetch_req_buffer.front().active) {

	prefetch_req& pre_q = prefetch_req_buffer.front();

        // schedule for page transfers from the active prefetch request when there is no pending transfer for the same
        // can be the very first time or a scheduled big chunk of pages (2MB) is finsihed just now
	if ( pre_q.pending_prefetch.empty() ) {

	     // case when the last schedule finished, it is not the first time
	     if( pre_q.cur_addr > pre_q.start_addr ) {

		 // all the memory fetches created by core on page fault were aggreagted earlier
                 // now they are replayed back together to the core
	         for( map<mem_addr_t, std::list<mem_fetch*> >::iterator iter = pre_q.outgoing_replayable_nacks.begin();
		      iter != pre_q.outgoing_replayable_nacks.end(); iter++) {

		      for(std::list<mem_fetch*>::iterator iter2 = iter->second.begin();
			  iter2 != iter->second.end(); iter2++) {

		      	  mem_fetch* mf = *iter2;
                        
		          simt_cluster_id = mf->get_sid() / m_config.num_core_per_cluster();
		          // push them to the upward queue to replay them back to the corresponding core in bulk
                          (m_gpu->getSIMTCluster(simt_cluster_id))->push_gmmu_cu_queue(mf);
		      }
	    	 }
		 pre_q.outgoing_replayable_nacks.clear();
	     }
	    
             // all the memory fetches have been replayed and
	     // the prefetch request is completed entirely
             // now signal the stream that the operation is finished so that it can schedule something else 
	     if( pre_q.cur_addr == pre_q.start_addr + pre_q.size ) {

	    	 pre_q.m_stream->record_next_done();
	    	 prefetch_req_buffer.pop_front();
		 return ;
	     }
	   
	     // break the loop if 
             //  Case 1: reach the end of this prefetch
             //  Case 2: it reaches the 2MB line from starting of the allocation
             //  Case 3: it encounters a valid page in between
	     do {
                    // get the page number for the current updated address
		    mem_addr_t page_num = m_gpu->get_global_memory()->get_page_num(pre_q.cur_addr);

                    // update the current address by page size as we break a big chunk (2MB) 
                    // in the granularity of the smallest unit of page
	            pre_q.cur_addr += m_config.page_size;

		    // check for Case 3, i.e., we encounter a valid page
		    if(  m_gpu->get_global_memory()->is_valid( page_num ) ) {

			 // check if this page is currently written back
			 check_write_stage_queue(page_num);

			 // break out of loop only when we have already scheduled some pages for transfer
                         // if not we will continue skipping valid pages if any until we find some invalid pages to transfer
                         if( !pre_q.pending_prefetch.empty() ) { 
                             break;
                     	 }    
		    } else {

                         // remember this page as pending under the prefetch request
		         pre_q.pending_prefetch.push_back(page_num);
			 
			 // just create a placeholder in MSHR for the memory fetches created by core on page fault
                         // later in the time so that they go to outgoing replayable nacks, rather than incoming 
			 req_info[page_num];

			 // incoming nacks hold the list of page faults for the transfer which has not been scheduled yet
                         // so instead of pushing them to MSHR and then again getting back to the outgoing list
                         // directly switch between the incoming and outgoing list of replayable nacks
			 if ( pre_q.incoming_replayable_nacks.find(page_num) != pre_q.incoming_replayable_nacks.end() ) {
			      pre_q.outgoing_replayable_nacks[page_num].merge(pre_q.incoming_replayable_nacks[page_num]);
			      pre_q.incoming_replayable_nacks.erase( page_num );
			 }

			 // current prefetch can cause eviction 
		         if ( m_gpu->get_global_memory()->should_evict_page(pcie_read_stage_queue.size(), m_config.free_page_buffer_percentage) ) {
                              page_eviction_procedure();
                    	 }    
 
			 // schedule this page as it is not valid to the read stage queue
                     	 pcie_read_stage_queue.push_back(page_num);
                    }
		    
	     } while( pre_q.cur_addr != (pre_q.start_addr + pre_q.size) && // check for Case 1, i.e., we reached the end of prefetch request
                      ((unsigned long long)(pre_q.cur_addr - pre_q.allocation_addr)) % ((unsigned long long)MAX_PREFETCH_SIZE) ); // Case 2: allowing maximum transfer size as huge page size of 2MB
	} 
    }
}

void gpgpu_sim::cycle()
{
   int clock_mask = next_clock_domain();

   // the gmmu has the same clock as the core
   if (clock_mask & GMMU) { 
       m_gmmu->cycle();
   }

   if (clock_mask & CORE ) {
       // shader core loading (pop from ICNT into core) follows CORE clock
      for (unsigned i=0;i<m_shader_config->n_simt_clusters;i++) 
         m_cluster[i]->icnt_cycle(); 
   }
    unsigned partiton_replys_in_parallel_per_cycle = 0;
    if (clock_mask & ICNT) {
        // pop from memory controller to interconnect
        for (unsigned i=0;i<m_memory_config->m_n_mem_sub_partition;i++) {
            mem_fetch* mf = m_memory_sub_partition[i]->top();
            if (mf) {
                unsigned response_size = mf->get_is_write()?mf->get_ctrl_size():mf->size();
                if ( ::icnt_has_buffer( m_shader_config->mem2device(i), response_size ) ) {
                    if (!mf->get_is_write()) 
                       mf->set_return_timestamp(gpu_sim_cycle+gpu_tot_sim_cycle);
                    mf->set_status(IN_ICNT_TO_SHADER,gpu_sim_cycle+gpu_tot_sim_cycle);
                    ::icnt_push( m_shader_config->mem2device(i), mf->get_tpc(), mf, response_size );
                    m_memory_sub_partition[i]->pop();
                    partiton_replys_in_parallel_per_cycle++;
                } else {
                    gpu_stall_icnt2sh++;
                }
            } else {
               m_memory_sub_partition[i]->pop();
            }
        }
    }
    partiton_replys_in_parallel += partiton_replys_in_parallel_per_cycle;

   if (clock_mask & DRAM) {
      for (unsigned i=0;i<m_memory_config->m_n_mem;i++){
         m_memory_partition_unit[i]->dram_cycle(); // Issue the dram command (scheduler + delay model)
         // Update performance counters for DRAM
         m_memory_partition_unit[i]->set_dram_power_stats(m_power_stats->pwr_mem_stat->n_cmd[CURRENT_STAT_IDX][i], m_power_stats->pwr_mem_stat->n_activity[CURRENT_STAT_IDX][i],
                        m_power_stats->pwr_mem_stat->n_nop[CURRENT_STAT_IDX][i], m_power_stats->pwr_mem_stat->n_act[CURRENT_STAT_IDX][i], m_power_stats->pwr_mem_stat->n_pre[CURRENT_STAT_IDX][i],
                        m_power_stats->pwr_mem_stat->n_rd[CURRENT_STAT_IDX][i], m_power_stats->pwr_mem_stat->n_wr[CURRENT_STAT_IDX][i], m_power_stats->pwr_mem_stat->n_req[CURRENT_STAT_IDX][i]);
      }
   }

   // L2 operations follow L2 clock domain
   unsigned partiton_reqs_in_parallel_per_cycle = 0;
   if (clock_mask & L2) {
       m_power_stats->pwr_mem_stat->l2_cache_stats[CURRENT_STAT_IDX].clear();
      for (unsigned i=0;i<m_memory_config->m_n_mem_sub_partition;i++) {
          //move memory request from interconnect into memory partition (if not backed up)
          //Note:This needs to be called in DRAM clock domain if there is no L2 cache in the system
          if ( m_memory_sub_partition[i]->full() ) {
             gpu_stall_dramfull++;
          } else {
              mem_fetch* mf = (mem_fetch*) icnt_pop( m_shader_config->mem2device(i) );
              m_memory_sub_partition[i]->push( mf, gpu_sim_cycle + gpu_tot_sim_cycle );
              partiton_reqs_in_parallel_per_cycle++;
          }
          m_memory_sub_partition[i]->cache_cycle(gpu_sim_cycle+gpu_tot_sim_cycle);
          m_memory_sub_partition[i]->accumulate_L2cache_stats(m_power_stats->pwr_mem_stat->l2_cache_stats[CURRENT_STAT_IDX]);
       }
   }
   partiton_reqs_in_parallel += partiton_reqs_in_parallel_per_cycle;
   if(partiton_reqs_in_parallel_per_cycle > 0){
	   partiton_reqs_in_parallel_util += partiton_reqs_in_parallel_per_cycle;
	   gpu_sim_cycle_parition_util++;
   }

   if (clock_mask & ICNT) {
      icnt_transfer();
   }

   if (clock_mask & CORE) {
      // L1 cache + shader core pipeline stages
      m_power_stats->pwr_mem_stat->core_cache_stats[CURRENT_STAT_IDX].clear();
      for (unsigned i=0;i<m_shader_config->n_simt_clusters;i++) {
         if (m_cluster[i]->get_not_completed() || get_more_cta_left() ) {
               m_cluster[i]->core_cycle();
               *active_sms+=m_cluster[i]->get_n_active_sms();
         }
         // Update core icnt/cache stats for GPUWattch
         m_cluster[i]->get_icnt_stats(m_power_stats->pwr_mem_stat->n_simt_to_mem[CURRENT_STAT_IDX][i], m_power_stats->pwr_mem_stat->n_mem_to_simt[CURRENT_STAT_IDX][i]);
         m_cluster[i]->get_cache_stats(m_power_stats->pwr_mem_stat->core_cache_stats[CURRENT_STAT_IDX]);
      }
      float temp=0;
      for (unsigned i=0;i<m_shader_config->num_shader();i++){
        temp+=m_shader_stats->m_pipeline_duty_cycle[i];
      }
      temp=temp/m_shader_config->num_shader();
      *average_pipeline_duty_cycle=((*average_pipeline_duty_cycle)+temp);
        //cout<<"Average pipeline duty cycle: "<<*average_pipeline_duty_cycle<<endl;


      if( g_single_step && ((gpu_sim_cycle+gpu_tot_sim_cycle) >= g_single_step) ) {
          raise(SIGTRAP); // Debug breakpoint
      }
      gpu_sim_cycle++;
      if( g_interactive_debugger_enabled ) 
         gpgpu_debug();

      // McPAT main cycle (interface with McPAT)
#ifdef GPGPUSIM_POWER_MODEL
      if(m_config.g_power_simulation_enabled){
          mcpat_cycle(m_config, getShaderCoreConfig(), m_gpgpusim_wrapper, m_power_stats, m_config.gpu_stat_sample_freq, gpu_tot_sim_cycle, gpu_sim_cycle, gpu_tot_sim_insn, gpu_sim_insn);
      }
#endif

      issue_block2core();
      
      // Depending on configuration, flush the caches once all of threads are completed.
      int all_threads_complete = 1;
      if (m_config.gpgpu_flush_l1_cache) {
         for (unsigned i=0;i<m_shader_config->n_simt_clusters;i++) {
            if (m_cluster[i]->get_not_completed() == 0)
                m_cluster[i]->cache_flush();
            else
               all_threads_complete = 0 ;
         }
      }

      if(m_config.gpgpu_flush_l2_cache){
          if(!m_config.gpgpu_flush_l1_cache){
              for (unsigned i=0;i<m_shader_config->n_simt_clusters;i++) {
                  if (m_cluster[i]->get_not_completed() != 0){
                      all_threads_complete = 0 ;
                      break;
                  }
              }
          }

         if (all_threads_complete && !m_memory_config->m_L2_config.disabled() ) {
            printf("Flushed L2 caches...\n");
            if (m_memory_config->m_L2_config.get_num_lines()) {
               int dlc = 0;
               for (unsigned i=0;i<m_memory_config->m_n_mem;i++) {
                  dlc = m_memory_sub_partition[i]->flushL2();
                  assert (dlc == 0); // need to model actual writes to DRAM here
                  printf("Dirty lines flushed from L2 %d is %d\n", i, dlc  );
               }
            }
         }
      }

      if (!(gpu_sim_cycle % m_config.gpu_stat_sample_freq)) {
         time_t days, hrs, minutes, sec;
         time_t curr_time;
         time(&curr_time);
         unsigned long long  elapsed_time = MAX(curr_time - g_simulation_starttime, 1);
         if ( (elapsed_time - last_liveness_message_time) >= m_config.liveness_message_freq ) {
            days    = elapsed_time/(3600*24);
            hrs     = elapsed_time/3600 - 24*days;
            minutes = elapsed_time/60 - 60*(hrs + 24*days);
            sec = elapsed_time - 60*(minutes + 60*(hrs + 24*days));

            DPRINTF(LIVENESS, "GPGPU-Sim uArch: cycles simulated: %lld  inst.: %lld (ipc=%4.1f) sim_rate=%u (inst/sec) elapsed = %u:%u:%02u:%02u / %s", 
                   gpu_tot_sim_cycle + gpu_sim_cycle, gpu_tot_sim_insn + gpu_sim_insn, 
                   (double)gpu_sim_insn/(double)gpu_sim_cycle,
                   (unsigned)((gpu_tot_sim_insn+gpu_sim_insn) / elapsed_time),
                   (unsigned)days,(unsigned)hrs,(unsigned)minutes,(unsigned)sec,
                   ctime(&curr_time));
            fflush(stdout);
            last_liveness_message_time = elapsed_time; 
         }
         visualizer_printstat();
         m_memory_stats->memlatstat_lat_pw();
         if (m_config.gpgpu_runtime_stat && (m_config.gpu_runtime_stat_flag != 0) ) {
            if (m_config.gpu_runtime_stat_flag & GPU_RSTAT_BW_STAT) {
               for (unsigned i=0;i<m_memory_config->m_n_mem;i++) 
                  m_memory_partition_unit[i]->print_stat(stdout);
               printf("maxmrqlatency = %d \n", m_memory_stats->max_mrq_latency);
               printf("maxmflatency = %d \n", m_memory_stats->max_mf_latency);
            }
            if (m_config.gpu_runtime_stat_flag & GPU_RSTAT_SHD_INFO) 
               shader_print_runtime_stat( stdout );
            if (m_config.gpu_runtime_stat_flag & GPU_RSTAT_L1MISS) 
               shader_print_l1_miss_stat( stdout );
            if (m_config.gpu_runtime_stat_flag & GPU_RSTAT_SCHED) 
               shader_print_scheduler_stat( stdout, false );
         }
      }

      if (!(gpu_sim_cycle % 20000)) {
         // deadlock detection 
         if (m_config.gpu_deadlock_detect && gpu_sim_insn == last_gpu_sim_insn) {
            gpu_deadlock = true;
         } else {
            last_gpu_sim_insn = gpu_sim_insn;
         }
      }
      try_snap_shot(gpu_sim_cycle);
      spill_log_to_file (stdout, 0, gpu_sim_cycle);

#if (CUDART_VERSION >= 5000)
      //launch device kernel
      launch_one_device_kernel();
#endif
   }
}


void shader_core_ctx::dump_warp_state( FILE *fout ) const
{
   fprintf(fout, "\n");
   fprintf(fout, "per warp functional simulation status:\n");
   for (unsigned w=0; w < m_config->max_warps_per_shader; w++ ) 
       m_warp[w].print(fout);
}

void gpgpu_sim::dump_pipeline( int mask, int s, int m ) const
{
/*
   You may want to use this function while running GPGPU-Sim in gdb.
   One way to do that is add the following to your .gdbinit file:
 
      define dp
         call g_the_gpu.dump_pipeline_impl((0x40|0x4|0x1),$arg0,0)
      end
 
   Then, typing "dp 3" will show the contents of the pipeline for shader core 3.
*/

   printf("Dumping pipeline state...\n");
   if(!mask) mask = 0xFFFFFFFF;
   for (unsigned i=0;i<m_shader_config->n_simt_clusters;i++) {
      if(s != -1) {
         i = s;
      }
      if(mask&1) m_cluster[m_shader_config->sid_to_cluster(i)]->display_pipeline(i,stdout,1,mask & 0x2E);
      if(s != -1) {
         break;
      }
   }
   if(mask&0x10000) {
      for (unsigned i=0;i<m_memory_config->m_n_mem;i++) {
         if(m != -1) {
            i=m;
         }
         printf("DRAM / memory controller %u:\n", i);
         if(mask&0x100000) m_memory_partition_unit[i]->print_stat(stdout);
         if(mask&0x1000000)   m_memory_partition_unit[i]->visualize();
         if(mask&0x10000000)   m_memory_partition_unit[i]->print(stdout);
         if(m != -1) {
            break;
         }
      }
   }
   fflush(stdout);
}

const struct shader_core_config * gpgpu_sim::getShaderCoreConfig()
{
   return m_shader_config;
}

const struct memory_config * gpgpu_sim::getMemoryConfig()
{
   return m_memory_config;
}

simt_core_cluster * gpgpu_sim::getSIMTCluster(int index)
{
   return *(m_cluster + index);
}

gmmu_t * gpgpu_sim::getGmmu()
{
   return m_gmmu;
}

