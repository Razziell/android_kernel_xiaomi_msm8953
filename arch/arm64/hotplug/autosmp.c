/*
 * arch/arm/kernel/autosmp.c
 *
 * automatically hotplug/unplug multiple cpu cores
 * based on cpu load and suspend state
 *
 * based on the msm_mpdecision code by
 * Copyright (c) 2012-2013, Dennis Rassmann <showp1984@gmail.com>
 *
 * Copyright (C) 2013-2014, Rauf Gungor, http://github.com/mrg666
 * rewrite to simplify and optimize, Jul. 2013, http://goo.gl/cdGw6x
 * optimize more, generalize for n cores, Sep. 2013, http://goo.gl/448qBz
 * generalize for all arch, rename as autosmp, Dec. 2013, http://goo.gl/x5oyhy
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version. For more details, see the GNU
 * General Public License included with the Linux kernel or available
 * at www.gnu.org/licenses
 */

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/input/state_notifier.h>

#define AUTOSMP			"autosmp"

#define HOTPLUG_ENABLED		(0)
#define HOTPLUG_INIT_DELAY	(30000)

static int enabled = HOTPLUG_ENABLED;

static struct delayed_work asmp_work;
static struct workqueue_struct *asmp_wq;
static struct kobject *asmp_kobj;

static struct asmp_tunables {
	int delay;
	int scroff_single_core;
	int max_cpus;
	int min_cpus;
	int cpufreq_up;
	int cpufreq_down;
	int cycle_up;
	int cycle_down;
} tunables = {
	.delay = 50,
	.scroff_single_core = 1,
	.max_cpus = CONFIG_NR_CPUS,
	.min_cpus = 2,
	.cpufreq_up = 60,
	.cpufreq_down = 40,
	.cycle_up = 2,
	.cycle_down = 2,
};

static unsigned int cycle = 0;

static void __ref asmp_work_fn(struct work_struct *work)
{
	unsigned int cpu = 0, slow_cpu = 0;
	unsigned int rate, cpu0_rate, slow_rate = UINT_MAX, fast_rate;
	unsigned int max_rate, up_rate, down_rate;
	int num_cpus;

	cycle++;

	max_rate = cpufreq_quick_get_max(cpu);

	up_rate = tunables.cpufreq_up * max_rate / 100;
	down_rate = tunables.cpufreq_down * max_rate / 100;

	get_online_cpus();
	num_cpus = num_online_cpus();

	cpu0_rate = cpufreq_quick_get(cpu);
	fast_rate = cpu0_rate;

	for_each_online_cpu(cpu) {
		if (cpu > 0) {
			rate = cpufreq_quick_get(cpu);
			if (rate <= slow_rate) {
				slow_cpu = cpu;
				slow_rate = rate;
			} else if (rate > fast_rate) {
				fast_rate = rate;
			}
		}
	}
	put_online_cpus();

	if (cpu0_rate < slow_rate)
		slow_rate = cpu0_rate;

	if (slow_rate > up_rate) {
		if ((num_cpus < tunables.max_cpus) &&
		    (cycle >= tunables.cycle_up)) {
			cpu = cpumask_next_zero(0, cpu_online_mask);
			cpu_up(cpu);
			cycle = 0;
		}
	} else if (slow_cpu && (fast_rate < down_rate)) {
		if ((num_cpus > tunables.min_cpus) &&
		    (cycle >= tunables.cycle_down)) {
 			cpu_down(slow_cpu);
			cycle = 0;
		}
	}

	queue_delayed_work(asmp_wq, &asmp_work,
		msecs_to_jiffies(tunables.delay));
}

static void asmp_suspend(void)
{
	unsigned int cpu;

	if (!enabled)
		return;

	if (tunables.scroff_single_core) {
		for_each_present_cpu(cpu) {
			if (cpu > 0 && cpu_online(cpu))
				cpu_down(cpu);
		}
	}

	cancel_delayed_work_sync(&asmp_work);

	pr_info("%s: suspended\n", __func__);
}

static void asmp_resume(void)
{
	unsigned int cpu, num_cpus;

	if (!enabled)
		return;

	if (tunables.scroff_single_core) {
		num_cpus = num_online_cpus();

		for_each_present_cpu(cpu) {
			if (num_cpus >= tunables.max_cpus)
				break;
			if (!cpu_online(cpu))
				cpu_up(cpu);
		}
	}

	queue_delayed_work(asmp_wq, &asmp_work,
		msecs_to_jiffies(tunables.delay));

	pr_info("%s: resumed\n", __func__);
}

static int __ref state_notifier_callback(struct notifier_block *this,
				   unsigned long event, void *data)
{
	if (!enabled)
		goto out;

	switch (event) {
	case STATE_NOTIFIER_ACTIVE:
		asmp_resume();
		break;
	case STATE_NOTIFIER_SUSPEND:
		asmp_suspend();
		break;
	}

out:
	return NOTIFY_OK;
}

static struct notifier_block asmp_suspend_notif = {
	.notifier_call = state_notifier_callback,
};

static int __ref asmp_hotplug_start(void)
{
	int ret = -EFAULT;

	asmp_wq = alloc_workqueue("asmp_hp_wq", WQ_HIGHPRI, 0);
	if (!asmp_wq) {
		pr_err("%s: unable to allocate workqueue\n", __func__);
		goto fail_workqueue;
	}

	ret = state_register_client(&asmp_suspend_notif);
	if (ret) {
		pr_err("%s: unable to register state notifier\n", __func__);
		goto fail_notifier;
	}

	INIT_DELAYED_WORK(&asmp_work, asmp_work_fn);
	queue_delayed_work(asmp_wq, &asmp_work,
		msecs_to_jiffies(HOTPLUG_INIT_DELAY));

	return ret;

fail_notifier:
	destroy_workqueue(asmp_wq);
fail_workqueue:
	return ret;
}

static void __ref asmp_hotplug_stop(void)
{
	unsigned int cpu;

	flush_workqueue(asmp_wq);
	cancel_delayed_work_sync(&asmp_work);

	state_unregister_client(&asmp_suspend_notif);
	asmp_suspend_notif.notifier_call = NULL;

	destroy_workqueue(asmp_wq);

	for_each_cpu_not(cpu, cpu_online_mask) {
		if (cpu == 0)
			continue;
		cpu_up(cpu);
	}
}

#define show_one(name)							\
static ssize_t show_##name(struct device *dev,				\
			   struct device_attribute *attr,		\
			   char *buf)					\
{									\
	return scnprintf(buf, SZ_64, "%d\n", tunables.name);		\
}

#define store_one(name, down_limit, up_limit)				\
static ssize_t store_##name(struct device *dev,				\
			    struct device_attribute *attr,		\
			    const char *buf, size_t count)		\
{									\
	int ret, val;							\
									\
	ret = sscanf(buf, "%d", &val);					\
	if (ret != 1 || val < down_limit || val > up_limit)		\
		return -EINVAL;						\
									\
	tunables.name = val;						\
									\
	return count;							\
}

#define create_one(name)						\
static DEVICE_ATTR(name, S_IWUSR | S_IRUGO,				\
	show_##name, store_##name);

static ssize_t show_enabled(struct device *dev,
			    struct device_attribute *attr,
			    char *buf)
{
	return scnprintf(buf, SZ_16, "%d\n", enabled);
}

static ssize_t store_enabled(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	int ret, val;

	ret = sscanf(buf, "%d", &val);
	if (ret != 1 || val < 0 || val > 1 || val == enabled)
		return -EINVAL;

	enabled = val;
	if (enabled)
		asmp_hotplug_start();
	else
		asmp_hotplug_stop();

	return count;
}

show_one(delay);
show_one(scroff_single_core);
show_one(min_cpus);
show_one(max_cpus);
show_one(cpufreq_up);
show_one(cpufreq_down);
show_one(cycle_up);
show_one(cycle_down);

store_one(delay, 10, 10000);
store_one(scroff_single_core, 0, 1);
store_one(min_cpus, 1, tunables.max_cpus);
store_one(max_cpus, tunables.min_cpus, 8);
store_one(cpufreq_up, 1, 100);
store_one(cpufreq_down, 1, 100);
store_one(cycle_up, 1, 6);
store_one(cycle_down, 1, 6);

create_one(enabled);
create_one(delay);
create_one(scroff_single_core);
create_one(min_cpus);
create_one(max_cpus);
create_one(cpufreq_up);
create_one(cpufreq_down);
create_one(cycle_up);
create_one(cycle_down);

static struct attribute *asmp_attrs[] = {
	&dev_attr_enabled.attr,
	&dev_attr_delay.attr,
	&dev_attr_scroff_single_core.attr,
	&dev_attr_min_cpus.attr,
	&dev_attr_max_cpus.attr,
	&dev_attr_cpufreq_up.attr,
	&dev_attr_cpufreq_down.attr,
	&dev_attr_cycle_up.attr,
	&dev_attr_cycle_down.attr,
	NULL,
};

static struct attribute_group attr_group = {
	.attrs = asmp_attrs,
	.name = "conf",
};

static int asmp_probe(struct platform_device *pdev)
{
	int ret = -EFAULT;

	asmp_kobj = kobject_create_and_add(AUTOSMP, kernel_kobj);
	if (!asmp_kobj) {
		pr_err("%s: unable to create kernel object\n", __func__);
		goto out;
	}

	ret = sysfs_create_group(asmp_kobj, &attr_group);
	if (ret) {
		pr_info("%s: unable to create sysfs\n", __func__);
		goto fail;
	}

	if (!enabled)
		goto out;

	ret = asmp_hotplug_start();
	if (ret) {
		pr_err("%s: unable to start hotplug\n", __func__);
		goto fail;
	}

	return ret;

fail:
	kobject_put(asmp_kobj);
out:
	return ret;
}

static int asmp_remove(struct platform_device *pdev)
{
	if (enabled)
		asmp_hotplug_stop();
	if (asmp_kobj)
		kobject_put(asmp_kobj);

	return 0;
}

static struct platform_driver asmp_driver = {
	.probe = asmp_probe,
	.remove = asmp_remove,
	.driver = {
		.name = AUTOSMP,
		.owner = THIS_MODULE,
	},
};

static struct platform_device asmp_device = {
	.name = AUTOSMP,
};

static int __init asmp_init(void)
{
	int ret;

	ret  = platform_driver_register(&asmp_driver);
	ret |= platform_device_register(&asmp_device);
	if (ret) {
		pr_err("%s: unable to register platform driver\n", __func__);
		goto fail;
	}

	pr_info("%s: registered\n", __func__);

fail:
	return ret;
}

static void __exit asmp_exit(void)
{
	platform_device_unregister(&asmp_device);
	platform_driver_unregister(&asmp_driver);
}

late_initcall(asmp_init);
module_exit(asmp_exit);
