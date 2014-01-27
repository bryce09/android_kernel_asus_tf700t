/*
 * Copyright (c) 2013, Francisco Franco <franciscofranco.1990@gmail.com>. 
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Simple no bullshit hot[un]plug driver for SMP
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/earlysuspend.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/hotplug.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/clk.h>



#include </usr/src/cm10.1/kernel/asus/tf700t/arch/arm/mach-tegra/cpu-tegra.h>
#include </usr/src/cm10.1/kernel/asus/tf700t/arch/arm/mach-tegra/clock.h>

extern int clk_set_parent(struct clk *c, struct clk *parent);

static struct clk *cpu_clk;
static struct clk *cpu_g_clk;
static struct clk *cpu_lp_clk;

#define DEFAULT_FIRST_LEVEL 60
#define HIGH_LOAD_COUNTER 20
#define TIMER HZ

#define MIN_TIME_CPU_ONLINE 1*HZ
#define MIN_TIME_G_ONLINE 3*HZ


static struct cpu_stats
{
    unsigned int default_first_level;
    unsigned int counter[2];
    unsigned long timestamp[3];
	unsigned int min_time_g_cluster;
	bool g_cluster_active;
} stats = {
    .default_first_level = DEFAULT_FIRST_LEVEL,
	.min_time_g_cluster = MIN_TIME_G_ONLINE,
    .counter = {0},
    .timestamp = {0},
};

struct cpu_load_data {
        u64 prev_cpu_idle;
        u64 prev_cpu_wall;
};

static DEFINE_PER_CPU(struct cpu_load_data, cpuload);

static struct workqueue_struct *wq;
static struct workqueue_struct *pm_wq;
static struct delayed_work decide_hotplug;
static struct work_struct resume;
static struct work_struct suspend;

static DEFINE_MUTEX(hotplug_lock);

bool is_g_cluster()
{
	bool ret;
	mutex_lock(&hotplug_lock);
	ret = stats.g_cluster_active;
	mutex_unlock(&hotplug_lock);
	return ret;
}


static inline int get_cpu_load(unsigned int cpu)
{
        struct cpu_load_data *pcpu = &per_cpu(cpuload, cpu);
        struct cpufreq_policy policy;
        u64 cur_wall_time, cur_idle_time;
        unsigned int idle_time, wall_time;
        unsigned int cur_load;

        cpufreq_get_policy(&policy, cpu);

        cur_idle_time = get_cpu_idle_time(cpu, &cur_wall_time, true);

        wall_time = (unsigned int) (cur_wall_time - pcpu->prev_cpu_wall);
        pcpu->prev_cpu_wall = cur_wall_time;

        idle_time = (unsigned int) (cur_idle_time - pcpu->prev_cpu_idle);
        pcpu->prev_cpu_idle = cur_idle_time;

        if (unlikely(!wall_time || wall_time < idle_time))
                return 0;

        cur_load = 100 * (wall_time - idle_time) / wall_time;

		if(is_g_cluster()) 
			return (cur_load * policy.cur) / (620*1000);
		
        return (cur_load * policy.cur) / policy.max;
}

 void g_cluster_revive()
{
	int cpu;

	if(!clk_set_parent(cpu_clk, cpu_g_clk)) 
	{
		/*catch-up with governor target speed */
		tegra_cpu_set_speed_cap(NULL);
		/* process pending core requests*/
#ifdef DEBUG_HOTPLUG
		pr_info("Running G Cluster\n");
#endif
	}
	mutex_lock(&hotplug_lock);
	for(cpu = 0; cpu < 2; cpu++) 
    {
        if (!cpu_online(cpu)) 
        {
            cpu_up(cpu);
			stats.timestamp[cpu] = jiffies;
        }
    }
	stats.g_cluster_active = true;
	stats.timestamp[2] = jiffies;
	mutex_unlock(&hotplug_lock);
}

static void g_cluster_smash()
{
	int cpu = 0;

	if (time_is_after_jiffies(stats.timestamp[2] + stats.min_time_g_cluster))
                return;
	for_each_online_cpu(cpu) 
    {
        if (cpu) 
        {
            cpu_down(cpu);
        }
    }
	

	/* limit max frequency in order to enter lp mode */
	tegra_update_cpu_speed(475*1000);

	if(!clk_set_parent(cpu_clk, cpu_lp_clk)) 
	{
		/*catch-up with governor target speed */
		tegra_cpu_set_speed_cap(NULL);
		/* process pending core requests*/
#ifdef DEBUG_HOTPLUG
		pr_info("Running Lp core\n");
#endif
	}

	mutex_lock(&hotplug_lock);
	stats.counter[0] = 0;
	stats.counter[1] = 0;
	stats.g_cluster_active = false;
	mutex_unlock(&hotplug_lock);
}
static void cpu_revive(unsigned int cpu)
{
        cpu_up(cpu);
        stats.timestamp[cpu - 2] = jiffies;
}

static void cpu_smash(unsigned int cpu)
{
        /*
         * Let's not unplug this cpu unless its been online for longer than
         * 1sec to avoid consecutive ups and downs if the load is varying
         * closer to the threshold point.
         */
        if (time_is_after_jiffies(stats.timestamp[cpu - 2] + MIN_TIME_CPU_ONLINE))
                return;

        cpu_down(cpu);
        stats.counter[cpu - 2] = 0;
}

static void __ref decide_hotplug_func(struct work_struct *work)
{
    int cpu;
        int i;
        int cpu_nr = 2;
        unsigned int cur_load;
    
    for_each_online_cpu(cpu) 
    {
                cur_load = get_cpu_load(cpu);

                if (cur_load >= stats.default_first_level)
                {
						stats.timestamp[2]=jiffies;
                        if (likely(stats.counter[cpu] < HIGH_LOAD_COUNTER)) 
                                {stats.counter[cpu] += 2; }

                        if (cpu_is_offline(cpu_nr) && stats.counter[cpu] >= 10)
                                cpu_revive(cpu_nr);
						if(!is_g_cluster() && stats.counter[0] >= 10)
						{
								g_cluster_revive();
								break;
						}
                }

                else
                {
                        if (stats.counter[cpu])
                                --stats.counter[cpu];

                        if (cpu_online(cpu_nr) && stats.counter[cpu] < 10)
						{
                                cpu_smash(cpu_nr);
						}
						if ( is_g_cluster() && (stats.counter[0] + stats.counter[1] < 5))
								g_cluster_smash();
									
                }

                cpu_nr++;

                if (cpu)
                        break;
        }

re_queue:        
    queue_delayed_work(wq, &decide_hotplug, msecs_to_jiffies(TIMER));
}

static void suspend_func(struct work_struct *work)
{
        int cpu;

    /* cancel the hotplug work when the screen is off and flush the WQ */
        flush_workqueue(wq);
    cancel_delayed_work_sync(&decide_hotplug);

    pr_info("Early Suspend stopping Hotplug work...\n");
    
	if(is_g_cluster())
    	g_cluster_smash();	
}

static void __ref resume_func(struct work_struct *work)
{
        int cpu;

       
    pr_info("Cpulimit: Late resume - restore cpu%d max frequency.\n", 0);

    g_cluster_revive();
    
    pr_info("Late Resume starting Hotplug work...\n");
    queue_delayed_work(wq, &decide_hotplug, HZ * 2);
        
}

static void mako_hotplug_early_suspend(struct early_suspend *handler)
{         
    queue_work(pm_wq, &suspend);
}

static void mako_hotplug_late_resume(struct early_suspend *handler)
{  
        queue_work(pm_wq, &resume);
}

static struct early_suspend mako_hotplug_suspend =
{
    .level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1,
        .suspend = mako_hotplug_early_suspend,
        .resume = mako_hotplug_late_resume,
};

/* sysfs functions for external driver */
void update_first_level(unsigned int level)
{
    stats.default_first_level = level;
}

void update_min_time_g_cluster(unsigned int sec)
{
	stats.min_time_g_cluster = sec * TIMER;
}


unsigned int get_first_level()
{
    return stats.default_first_level;
}

unsigned int get_min_time_g_cluster()
{
	return stats.min_time_g_cluster/TIMER;
}

/* end sysfs functions from external driver */

int __init mako_hotplug_init(void)
{
    pr_info("Mako Hotplug driver started.\n");

	cpu_clk = clk_get_sys(NULL, "cpu");
	cpu_g_clk = clk_get_sys(NULL, "cpu_g");
	cpu_lp_clk = clk_get_sys(NULL, "cpu_lp");

	if (NULL ==(cpu_clk) || (cpu_g_clk)==NULL || (cpu_lp_clk)==NULL)
	{
		pr_info("Error getting cpu clocks....");
		return -ENOENT;
	}

    wq = alloc_ordered_workqueue("mako_hotplug_workqueue", 0);
    
    if (!wq)
        return -ENOMEM;

        pm_wq = alloc_workqueue("pm_workqueue", 0, 1);
    
    if (!pm_wq)
        return -ENOMEM;

        stats.timestamp[0] = jiffies;
        stats.timestamp[1] = jiffies;  
		stats.timestamp[2] = jiffies;
		stats.g_cluster_active = true;  


    INIT_DELAYED_WORK(&decide_hotplug, decide_hotplug_func);
        INIT_WORK(&resume, resume_func);
        INIT_WORK(&suspend, suspend_func);
    queue_delayed_work(wq, &decide_hotplug, HZ*25);
    
    register_early_suspend(&mako_hotplug_suspend);
    
    return 0;
}
late_initcall(mako_hotplug_init);
