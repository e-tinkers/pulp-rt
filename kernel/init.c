/*
 * Copyright (C) 2018 ETH Zurich and University of Bologna
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Authors: Germain Haugou, ETH (germain.haugou@iis.ee.ethz.ch)
 */

#include "rt.h"
#include "rt/rt_api.h"
#include "archi/pulp.h"

typedef void (*fptr)(void);

static fptr ctor_list[1] __attribute__((section(".ctors.start"))) = { (fptr) -1 };
static fptr dtor_list[1] __attribute__((section(".dtors.start"))) = { (fptr) -1 };

static int __rt_check_clusters_start();

RT_BOOT_CODE static void do_ctors(void)
{
  //plp_trace(RT_TRACE_INIT, "Constructors execution\n");
  fptr *fpp;

  for(fpp = ctor_list+1;  *fpp != 0;  ++fpp) {
    (**fpp)();
  }
  //plp_trace(RT_TRACE_INIT, "Constructors execution done\n");
}

RT_BOOT_CODE static void do_dtors(void)
{
  //plp_trace(RT_TRACE_INIT, "Destructors execution\n");
  fptr *fpp;
  for(fpp = dtor_list + 1;  *fpp != 0;  ++fpp) (**fpp)();
  //plp_trace(RT_TRACE_INIT, "Destructors execution done\n");
}

void __rt_putc_debug_bridge(char c);

#if PULP_CHIP == CHIP_GAP || PULP_CHIP == CHIP_VEGA
void __rt_pmu_init();
#endif

#ifdef __RT_MODE_BARE

void __rt_init()
{
}

void __rt_deinit()
{
}

#else

void __rt_init()
{
  // Notify the debug bridge that the chip is ready to be used
  hal_bridge_t *bridge = hal_bridge_get();
  bridge->target.available = 1;
  apb_soc_jtag_reg_write(apb_soc_jtag_reg_loc(apb_soc_jtag_reg_read()) | 2);

  rt_trace(RT_TRACE_INIT, "Starting runtime initialization\n");

#if defined(ARCHI_HAS_FC)
  // Deactivate all soc events as they are active by default
  soc_eu_eventMask_reset(SOC_FC_FIRST_MASK);
#endif


#ifndef __ariane__

#ifdef FLL_VERSION
#if PULP_CHIP == CHIP_GAP || PULP_CHIP == CHIP_VEGA
  __rt_pmu_init();
#endif

  // Initialize first the FLLs
  __rt_freq_init();
#endif

  if (rt_is_fc()) {
#if defined(ARCHI_FC_HAS_ICACHE)
    // Enable instruction cache, initialize all memories
    enable_all_icache_banks();
#endif
  } else {
#if defined(ARCHI_HAS_CLUSTER)
    // Enable instruction cache, initialize all memories
    enable_all_icache_banks();
#endif
  }

  __rt_irq_init();
  // Initialize first the memory allocators and the utils so that they are
  // available for constructors, especially to let them declare
  // callbacks
  __rt_utils_init();
  __rt_allocs_init();

  // Call global and static constructors
  // Each module may do private initializations there
  do_ctors();

  // Activate soc events handler
#if defined(ARCHI_HAS_FC)
  rt_irq_set_handler(ARCHI_FC_EVT_SOC_EVT, __rt_fc_socevents_handler);
  rt_irq_mask_set(1<<ARCHI_FC_EVT_SOC_EVT);
#endif

  rt_irq_enable();

  // Now do individual modules initializations.
  if (__rt_cbsys_exec(RT_CBSYS_START)) goto error;

  if (__rt_check_clusters_start()) goto error;

  return;

error:
  rt_fatal("There was an error during runtime initialization\n");
  exit(-1);

#else

#ifdef FLL_VERSION
  // Initialize first the FLLs
  __rt_freq_init();
#endif

#endif

}

void __rt_deinit()
{
#ifndef __ariane__

  // Stop all modules
  __rt_cbsys_exec(RT_CBSYS_STOP);

#endif

  /* Call global and static destructors */
  do_dtors();
}

#endif


#if defined(ARCHI_HAS_CLUSTER)

extern int main();

RT_L1_GLOBAL_DATA static int retval;
RT_L2_DATA void (*__rt_cluster_entry)(void *) = NULL;
RT_L2_DATA void *__rt_cluster_entry_arg;


static void cluster_pe_start(void *arg)
{
  rt_irq_enable();
  retval = main();
}

static void cluster_start(void *arg)
{

#if defined(EU_VERSION) && EU_VERSION >= 3

  eu_evt_maskSet((1<<PULP_DISPATCH_EVENT) | (1<<PULP_HW_BAR_EVENT) | (1<<PULP_MUTEX_EVENT));

  if (__rt_config_cluster_start())
  {
    if (__rt_cluster_entry != NULL)
    {
      __rt_cluster_entry(__rt_cluster_entry_arg);
    }
    else
    {
      rt_team_fork(rt_nb_active_pe(), cluster_pe_start, NULL);
    }
  }
  else
  {
    __rt_team_config(rt_nb_active_pe());
  }

#endif

}

static int cluster_master_start(void *arg)
{
  return retval;
}

static int __rt_check_cluster_start(int cid, rt_event_t *event)
{
  if (rt_cluster_id() != cid)
  {
    // Classic remote cluster start procedure
    rt_cluster_mount(1, cid, 0, NULL);
    void *stacks = rt_alloc(RT_ALLOC_CL_DATA+cid, rt_stack_size_get()*rt_nb_active_pe());
    if (stacks == NULL) return -1;

    if (rt_cluster_call(NULL, cid, cluster_start, NULL, stacks, rt_stack_size_get(), rt_stack_size_get(), rt_nb_active_pe(), event)) return -1;
  }
  else
  {
    // Local cluster start procedure in case we are running here
    rt_cluster_mount(1, cid, 0, NULL);
    void *stacks = rt_alloc(RT_ALLOC_CL_DATA+cid, rt_stack_size_get()*(rt_nb_active_pe()-1));

    if (stacks == NULL) return -1;

#if defined(EU_VERSION) && EU_VERSION >= 3
#ifndef ARCHI_HAS_NO_DISPATCH
    eu_dispatch_team_config((1<<rt_nb_active_pe())-1);
    eu_dispatch_push((unsigned int)__rt_set_slave_stack | 1);
    eu_dispatch_push((unsigned int)rt_stack_size_get());
    eu_dispatch_push((unsigned int)stacks);
#endif
#else
#if defined(__riscv__)
    __rt_cluster_pe_init(stacks, rt_stack_size_get());
    eoc_fetch_enable_remote(0, (1<<rt_nb_active_pe()) - 1);
#else
#endif
#endif

    cluster_start(NULL);
  }

  return 0;
}

static int __rt_check_clusters_start()
{
  if (__rt_config_cluster_start()) {
    // All fetch mode, starts all cluster
    if (rt_event_alloc(NULL, rt_nb_cluster())) return -1;

    rt_event_t *events[rt_nb_cluster()];

    for (int cid=0; cid<rt_nb_cluster(); cid++)
    {
      events[cid] = rt_event_get_blocking(NULL);
      if (__rt_check_cluster_start(cid, events[cid])) return -1;
    }
    if (rt_is_fc()) {
      if (__rt_config_fc_start())
      {
        int fc_retval = main();
        for (int cid=0; cid<rt_nb_cluster(); cid++)
        {
          rt_event_wait(events[cid]);
        }
        exit(retval != 0 || fc_retval != 0);
      }
      else
      {
        for (int cid=0; cid<rt_nb_cluster(); cid++)
        {
          rt_event_wait(events[cid]);
        }
        exit(retval);
      }
    }
    else
      return cluster_master_start(NULL);
  } else if (!rt_is_fc()) {
    // Otherwise just check cluster 0, in case we are running on it
    if (__rt_check_cluster_start(0, NULL)) return -1;
  }
  return 0;
}

#else

static int __rt_check_clusters_start()
{
  return 0;
}

#endif
