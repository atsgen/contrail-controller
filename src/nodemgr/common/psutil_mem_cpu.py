#
# Copyright (c) 2020 ATS, Inc. All rights reserved.
#

import sys
import os
import psutil
import subprocess

from subprocess import Popen, PIPE

from nodemgr.common.sandesh.nodeinfo.cpuinfo.ttypes import ProcessCpuInfo


class PsutilMemCpuUsageData(object):
    def __init__(self, _id, last_cpu, last_time):
        self.last_cpu = last_cpu
        self.last_time = last_time
        self._id = _id

    def _get_num_cpu(self):
        cmd = 'lscpu | grep "^CPU(s):" | awk \'{print $2}\''
        proc = Popen(cmd, shell=True, stdout=PIPE)
        return int(proc.communicate()[0])

    def _get_process_cpu_share(self):
        last_cpu = self.last_cpu
        last_time = self.last_time

        try:
            proc =  psutil.Process(self._id)
        except:
            return 0

        if hasattr(proc, 'get_cpu_times'):
            current_cpu = proc.get_cpu_times()
        else:
            current_cpu = proc.cpu_times()

        current_time = os.times()[4]
        for i in range(0, len(current_cpu)-1):
            current_time += current_cpu[i]

        # tracking system/user time only
        interval_time = 0
        if last_cpu and (last_time != 0):
            sys_time = current_cpu.system - last_cpu.system
            usr_time = current_cpu.user - last_cpu.user
            interval_time = current_time - last_time

        self.last_cpu = current_cpu
        self.last_time = current_time

        if interval_time > 0:
            sys_percent = 100 * sys_time / interval_time
            usr_percent = 100 * usr_time / interval_time
            cpu_share = round((sys_percent + usr_percent)/self._get_num_cpu(), 2)
            return cpu_share
        else:
            return 0

    def get_process_mem_cpu_info(self):
        process_mem_cpu = ProcessCpuInfo()
        process_mem_cpu.cpu_share = self._get_process_cpu_share()
        try:
            proc =  psutil.Process(self._id)
            if hasattr(proc, 'get_memory_info'):
                mem_stats = proc.get_memory_info()
            else:
                mem_stats = proc.memory_info()
            process_mem_cpu.mem_virt = mem_stats.vms/1024
            process_mem_cpu.mem_res = mem_stats.rss/1024
        except:
            process_mem_cpu.mem_virt = 0
            process_mem_cpu.mem_res = 0

        return process_mem_cpu
