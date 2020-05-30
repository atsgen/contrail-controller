#
# Copyright (c) 2020 ATS, Inc. All rights reserved
#

import psutil
import time
import os

from nodemgr.common.psutil_mem_cpu import PsutilMemCpuUsageData
from sandesh_common.vns.ttypes import Module
import nodemgr.common.common_process_manager as cpm
from pysandesh.gen_py.sandesh.ttypes import SandeshLevel

_unit_name_to_process_name = {
    Module.ANALYTICS_NODE_MGR: {
        'contrail-collector': 'contrail-collector',
        'contrail-analytics-api': 'contrail-analytics-api',
        'contrail-analytics-nodemgr': 'contrail-nodemgr'
    },
    Module.ANALYTICS_ALARM_NODE_MGR: {
        'contrail-alarm-gen': 'contrail-alarm-gen',
        'kafka': 'kafka',
        'contrail-analytics-alarm-nodemgr': 'contrail-nodemgr'
    },
    Module.ANALYTICS_SNMP_NODE_MGR: {
        'contrail-snmp-collector': 'contrail-snmp-collector',
        'contrail-topology': 'contrail-topology',
        'contrail-analytics-snmp-nodemgr': 'contrail-nodemgr'
    },
    Module.CONFIG_NODE_MGR: {
        'contrail-api': 'contrail-api',
        'contrail-schema': 'contrail-schema',
        'contrail-svc-monitor': 'contrail-svc-monitor',
        'contrail-device-manager': 'contrail-device-manager',
        'contrail-config-nodemgr': 'contrail-nodemgr'
    },
    Module.CONFIG_DATABASE_NODE_MGR: {
        'cassandra': 'cassandra',
        'contrail-config-database-nodemgr': 'contrail-nodemgr'
    },
    Module.CONTROL_NODE_MGR: {
        'contrail-control': 'contrail-control',
        'contrail-dns': 'contrail-dns',
        'contrail-named': 'contrail-named',
        'contrail-control-nodemgr': 'contrail-nodemgr'
    },
    Module.COMPUTE_NODE_MGR: {
        'contrail-vrouter-agent': 'contrail-vrouter-agent',
        'contrail-vrouter-nodemgr': 'contrail-nodemgr'
    },
    Module.DATABASE_NODE_MGR: {
        'contrail-query-engine': 'contrail-query-engine',
        'cassandra': 'cassandra',
        'contrail-database-nodemgr': 'contrail-nodemgr'
    },
}

class PsutilProcessInfoManager(object):
    def __init__(self, event_mgr, module_type, unit_names, event_handlers,
                 update_process_list):
        self._event_mgr = event_mgr
        self._module_type = module_type
        self._unit_names = unit_names
        self._event_handlers = event_handlers
        self._update_process_list = update_process_list
        self._process_info_cache = cpm.ProcessInfoCache()

    def _get_process_name(self, unit):
        names_map = _unit_name_to_process_name.get(self._module_type)
        return names_map.get(unit)

    def _poll_processes(self):
        for unit in self._unit_names:
            name = self._get_process_name(unit)
            if name is None:
                continue

            info = {}
            info['pid'] = -1
            info['statename'] = 'PROCESS_STATE_EXITED'
            info['name'] = unit
            info['group'] = unit
            found = False
            self._event_mgr.msg_log("searching for %s" % name,
                                    SandeshLevel.SYS_ERR)
            for proc in psutil.process_iter():
                cmdline = proc.cmdline()
                do_iter = False
                is_python = False
                pname = proc.name()
                if "python" in cmdline[0]:
                    if len(cmdline) > 1:
                        is_python = True
                        pname = cmdline[1]
                elif "java" in cmdline[0]:
                    do_iter = True

                if do_iter:
                    for c in cmdline:
                        if name in c:
                            found = True
                            break
                else:
                    if is_python:
                        found = name in pname
                    else:
                        found = name == pname
                if found:
                    info['pid'] = proc.pid
                    info['start'] = str(proc.create_time() * 1000000)
                    info['statename'] = 'PROCESS_STATE_RUNNING'
                    self._event_mgr.msg_log("found %s" % name,
                                            SandeshLevel.SYS_ERR)
                    break
            if self._process_info_cache.update_cache(info):
                self._event_handlers['PROCESS_STATE'](cpm.convert_to_pi_event(info))
                if self._update_process_list:
                    self._event_handlers['PROCESS_LIST_UPDATE']()

    def get_all_processes(self):
        processes_info_list = []
        for unit in self._unit_names:
            name = self._get_process_name(unit)
            if name is None:
                continue

            info = {}
            info['pid'] = -1
            info['statename'] = 'PROCESS_STATE_EXITED'
            info['name'] = unit
            info['group'] = unit
            found = False
            self._event_mgr.msg_log("searching for %s" % name,
                                    SandeshLevel.SYS_ERR)
            for proc in psutil.process_iter():
                cmdline = proc.cmdline()
                do_iter = False
                is_python = False
                pname = proc.name()
                if "python" in cmdline[0]:
                    if len(cmdline) > 1:
                        is_python = True
                        pname = cmdline[1]
                elif "java" in cmdline[0]:
                    do_iter = True

                if do_iter:
                    for c in cmdline:
                        if name in c:
                            found = True
                            break
                else:
                    if is_python:
                        found = name in pname
                    else:
                        found = name == pname
                if found:
                    info['pid'] = proc.pid
                    info['start'] = str(proc.create_time() * 1000000)
                    info['statename'] = 'PROCESS_STATE_RUNNING'
                    self._event_mgr.msg_log("found %s" % name,
                                            SandeshLevel.SYS_ERR)
                    break
            processes_info_list.append(info)
            self._process_info_cache.update_cache(info)
        return processes_info_list

    def run_job(self):
        self._poll_processes()

    def get_mem_cpu_usage_data(self, pid, last_cpu, last_time):
        return PsutilMemCpuUsageData(pid, last_cpu, last_time)

