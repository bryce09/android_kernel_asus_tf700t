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

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/platform_device.h>
#include <linux/timer.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/hotplug.h>
#include <linux/input.h>
#include <linux/jiffies.h>

#include <linux/clk.h>
#include </usr/src/cm10.1/kernel/asus/tf700t/arch/arm/mach-tegra/cpu-tegra.h>
#include </usr/src/cm10.1/kernel/asus/tf700t/arch/arm/mach-tegra/clock.h>

#include <linux/earlysuspend.h>

#define MAKO_HOTPLUG "mako_hotplug"

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
	atomic_t req_revive;
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
static struct delayed_work decide_hotplug;
static struct work_struct suspend;
static struct work_struct resume;



inline bool is_g_cluster()
{
	return stats.g_cluster_active;
}

void set_g_revive()
{
	atomic_set(&stats.req_revive, 1);
}
void set_g_revive_done()
{
	atomic_set(&stats.req_revive, 0);
}
inline int get_g_revive()
{
	
	return atomic_read(&stats.req_revive);
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
inline void set_timestamp_g()
{
	stats.timestamp[2] = jiffies;
}
inline unsigned long get_timestamp_g()
{
	unsigned long t;
	t = stats.timestamp[2];
	return t;
}

 void g_cluster_revive()
{
	int cpu;

	stats.g_cluster_active = true;
	if(!clk_set_parent(cpu_clk, cpu_g_clk)) 
	{
		/*catch-up with governor target speed */
		tegra_cpu_set_speed_cap(NULL);
		/* process pending core requests*/
#ifdef DEBUG_HOTPLUG
		pr_info("Running G Cluster\n");
#endif
	}
	
	for(cpu = 0; cpu < 2; cpu++) 
    {
        if (!cpu_online(cpu)) 
            cpu_up(cpu);
    }
	
	set_timestamp_g();
}

static void g_cluster_smash()
{
	int cpu = 0;

	
	if (time_is_after_jiffies(get_timestamp_g() + stats.min_time_g_cluster))
                return;

	stats.g_cluster_active = false;
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

	
	stats.counter[0] = 0;
	stats.counter[1] = 0;
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
        int cpu_nr = 2;
        unsigned int cur_load;

	if(!is_g_cluster() && get_g_revive() )
	{
		g_cluster_revive();
		set_g_revive_done();
	}
    for_each_online_cpu(cpu) 
    {
                cur_load = get_cpu_load(cpu);

                if (cur_load >= stats.default_first_level)
                {
                        if (likely(stats.counter[cpu] < HIGH_LOAD_COUNTER)) 
                                {stats.counter[cpu] += 2; }
						if( stats.counter[cpu] >= 10)
						{
							if(!is_g_cluster())
							{
								g_cluster_revive();
								break;
							}
							else if(cpu_is_offline(cpu_nr))
								cpu_revive(cpu_nr);
						}
						set_timestamp_g();
						
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
	       
    queue_delayed_work_on(0, wq, &decide_hotplug, msecs_to_jiffies(TIMER));
}

static void mako_hotplug_suspend(struct work_struct *work)
{
    int cpu;

    pr_info("Early Suspend stopping Hotplug work...\n");
    
	if(is_g_cluster())
    	g_cluster_smash();	
}

static void __ref mako_hotplug_resume(struct work_struct *work)
{
    int cpu;

    g_cluster_revive();       
}

static void mako_hotplug_early_suspend(struct early_suspend *handler)
{         
    schedule_work(&suspend);
}

static void mako_hotplug_late_resume(struct early_suspend *handler)
{  
    schedule_work(&resume);
}

static struct early_suspend early_suspend =
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

static int __devinit mako_hotplug_probe(struct platform_device *pdev)
{
        int ret = 0;

    wq = alloc_workqueue("mako_hotplug_workqueue", WQ_HIGHPRI | WQ_FREEZABLE, 0);
    
    if (!wq)
        {
                ret = -ENOMEM;
                goto err;
        }

		cpu_clk = clk_get_sys(NULL, "cpu");
		cpu_g_clk = clk_get_sys(NULL, "cpu_g");
		cpu_lp_clk = clk_get_sys(NULL, "cpu_lp");

		if (NULL ==(cpu_clk) || (cpu_g_clk)==NULL || (cpu_lp_clk)==NULL)
		{
			pr_info("Error getting cpu clocks....");
			return -ENOENT;
		}

        stats.timestamp[0] = jiffies;
        stats.timestamp[1] = jiffies;
		stats.timestamp[2] = jiffies;
		stats.g_cluster_active = true; 
		atomic_set(&stats.req_revive, 0);
        register_early_suspend(&early_suspend);

        INIT_WORK(&suspend, mako_hotplug_suspend);
        INIT_WORK(&resume, mako_hotplug_resume);
    INIT_DELAYED_WORK(&decide_hotplug, decide_hotplug_func);

        queue_delayed_work_on(0, wq, &decide_hotplug, HZ * 20);

err:
        return ret;        
}

static struct platform_device mako_hotplug_device = {
  .name = MAKO_HOTPLUG,
  .id = -1,
};

static int mako_hotplug_remove(struct platform_device *pdev)
{
	destroy_workqueue(wq);
	return 0;
}

static struct platform_driver mako_hotplug_driver = {
        .probe = mako_hotplug_probe,
        .remove = mako_hotplug_remove,
        .driver = {
                .name = MAKO_HOTPLUG,
                .owner = THIS_MODULE,
        },
};

static int __init mako_hotplug_init(void)
{
        int ret;

        ret = platform_driver_register(&mako_hotplug_driver);

        if (ret)
        {
                return ret;
        }

        ret = platform_device_register(&mako_hotplug_device);

        if (ret)
        {
                return ret;
        }

        pr_info("%s: init\n", MAKO_HOTPLUG);

        return ret;
}
static void __exit mako_hotplug_exit(void)
{
        platform_device_unregister(&mako_hotplug_device);
        platform_driver_unregister(&mako_hotplug_driver);
}

late_initcall(mako_hotplug_init);
module_exit(mako_hotplug_exit);
