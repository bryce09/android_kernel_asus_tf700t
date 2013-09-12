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
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/clk.h>
 
#include </usr/src/cm10.1/kernel/asus/tf700t/arch/arm/mach-tegra/pm.h>
#include </usr/src/cm10.1/kernel/asus/tf700t/arch/arm/mach-tegra/cpu-tegra.h>
#include </usr/src/cm10.1/kernel/asus/tf700t/arch/arm/mach-tegra/clock.h>

#define DEFAULT_FIRST_LEVEL 60
#define DEFAULT_CORES_ON_TOUCH 2
#define HIGH_LOAD_COUNTER 20
#define TIMER HZ


extern int clk_set_parent(struct clk *c, struct clk *parent);


static struct clk *cpu_clk;
static struct clk *cpu_g_clk;
static struct clk *cpu_lp_clk;

struct cpu_stats
{
    unsigned int total_cpus;
    unsigned int default_first_level;
    unsigned int suspend_frequency;
    unsigned int cores_on_touch;

    unsigned int counter[2];
};

static struct cpu_stats stats;
static struct workqueue_struct *wq;
static struct delayed_work decide_hotplug;

extern void tegra_exit_lp_mode(void);
extern void tegra_enter_lp_mode(void);

static void decide_hotplug_func(struct work_struct *work)
{
    int cpu;
    

#if 0
    if (unlikely(is_touching && num_online_cpus() < stats.cores_on_touch))
    {
        for_each_possible_cpu(cpu_boost)
        {
            if (!cpu_online(cpu_boost) && cpu_boost < stats.cores_on_touch) 
            {
                cpu_up(cpu_boost);
            }
        }
    }
#endif
    for_each_online_cpu(cpu) 
    {
        if (report_load_at_max_freq(cpu) >= stats.default_first_level)
        {
            if (likely(stats.counter[cpu] < HIGH_LOAD_COUNTER))    
                stats.counter[cpu] += 2;
        }

        else
        {
            if (stats.counter[cpu] > 0)
                stats.counter[cpu]--;
        }

        if (cpu) 
        {
            cpu = 0;
            break;
        }
    }
	
	if ((stats.counter[0] + stats.counter[1]) < 10)
	{
		if (cpu_online(1))
		{
			cpu_down(1);
		}
	}
    if (stats.counter[0] >= 10)
    {
		if (!cpu_online(1))
		{
			cpu_up(1);
		}
        if (!cpu_online(2))
        {
            cpu_up(2);
        }
    }
    else
    {
        if (cpu_online(2))
        {
            cpu_down(2);
        }   
    }
    
    if (stats.counter[1] >= 10)
    {
        if (!cpu_online(3))
        {
            cpu_up(3);
        }
    }

    else
    {
        if (cpu_online(3))
        {
            cpu_down(3);
        }   
    }
#ifdef DEBUG_HOTPLUG
	pr_info("HOTPLUG DEBUG\n");
	pr_info("0: stats.counter:\t%d", stats.counter[0]);
	pr_info("1: stats.counter:\t%d", stats.counter[1]);
	//pr_info("load_avg 0,1:\t%d", load_avg);
#endif
    queue_delayed_work(wq, &decide_hotplug, msecs_to_jiffies(TIMER));
}

static void mako_hotplug_early_suspend(struct early_suspend *handler)
{	 
    int cpu;

    /* cancel the hotplug work when the screen is off and flush the WQ */
    cancel_delayed_work_sync(&decide_hotplug);
    flush_workqueue(wq);

    pr_info("Early Suspend stopping Hotplug work...\n");
    
    for_each_online_cpu(cpu) 
    {
        if (cpu) 
        {
            cpu_down(cpu);
        }
    }
	
	/* limit max frequency in order to enter lp mode */
	tegra_update_cpu_speed(475);
	if(!clk_set_parent(cpu_clk, cpu_lp_clk)) 
	{
		/*catch-up with governor target speed */
		tegra_cpu_set_speed_cap(NULL);
		/* process pending core requests*/
#ifdef DEBUG_HOTPLUG
		pr_info("Running Lp core\n");
#endif
	}
	msleep(2000);
}


static void mako_hotplug_late_resume(struct early_suspend *handler)
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
	
	msleep(70);
    /* online 2 cores when the screen goes online */
    for(cpu = 0; cpu < 2; cpu++) 
    {
        if (!cpu_online(cpu)) 
        {
            cpu_up(cpu);
        }
    }

    stats.counter[0] = 0;
    stats.counter[1] = 0;

    pr_info("Late Resume starting Hotplug work...\n");
    queue_delayed_work(wq, &decide_hotplug, HZ);
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

void update_suspend_frequency(unsigned int freq)
{
    stats.suspend_frequency = freq;
}

void update_cores_on_touch(unsigned int num)
{
    stats.cores_on_touch = num;
}

unsigned int get_first_level()
{
    return stats.default_first_level;
}

unsigned int get_cores_on_touch()
{
    return stats.cores_on_touch;
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
    /* init everything here */
    stats.total_cpus = num_present_cpus();
    stats.default_first_level = DEFAULT_FIRST_LEVEL;
    stats.cores_on_touch = DEFAULT_CORES_ON_TOUCH;
    stats.counter[0] = 0;
    stats.counter[1] = 0;

    wq = alloc_workqueue("mako_hotplug_workqueue", 
                    WQ_UNBOUND | WQ_RESCUER | WQ_FREEZABLE, 1);
    
    if (!wq)
        return -ENOMEM;
    
    INIT_DELAYED_WORK(&decide_hotplug, decide_hotplug_func);
    queue_delayed_work(wq, &decide_hotplug, HZ*25);
    
    register_early_suspend(&mako_hotplug_suspend);
    
    return 0;
}
late_initcall(mako_hotplug_init);


