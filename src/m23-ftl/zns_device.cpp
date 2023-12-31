/*
 * MIT License
Copyright (c) 2021 - current
Authors:  Animesh Trivedi
This code is part of the Storage System Course at VU Amsterdam
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
 */

#include "zns_device.h"
#include "libnvme.h"
#include <cerrno>
#include <unordered_map>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

extern "C"
{

#define ENTRY_INVALID (1L << 63)
#define address_2_zone(addr) ((addr) / (zns_dev_ex->blocks_per_zone * zns_dev->lba_size_bytes)) + zns_dev_ex->log_zone_num_config
#define address_2_offset(addr) ((addr) % (zns_dev_ex->blocks_per_zone * zns_dev->lba_size_bytes) / zns_dev->lba_size_bytes)
#define zone_2_address(zone_no) (zone_no - zns_dev_ex->log_zone_num_config) * (zns_dev_ex->blocks_per_zone * zns_dev->lba_size_bytes)
#define map_contains(map, key) (map.find(key) != map.end())
#define EMPTY 1
#define FULL 14
#define MDTS (64 * 4096)
#define roundup(x, y) (                  \
    {                                    \
        typeof(y) __y = y;               \
        (((x) + (__y - 1)) / __y) * __y; \
    })

    std::unordered_map<int64_t, int64_t> log_mapping;
    std::unordered_map<int64_t, int64_t> data_mapping;

    struct user_zns_device *zns_dev;
    struct zns_device_extra_info *zns_dev_ex;

    int ss_nvme_device_io_with_mdts(uint64_t slba, void *buffer, uint64_t buf_size, bool read)
    {
        int ret;

        uint64_t size_left = buf_size, ptr = 0, io_num, wp = slba, lba_num, mdts_size = zns_dev_ex->mdts, lba_size = zns_dev->lba_size_bytes;
        __u64 res;
        while (size_left > 0)
        {
            io_num = mdts_size < size_left ? mdts_size : size_left;
            lba_num = io_num / lba_size - ((io_num % lba_size) == 0 ? 1 : 0);
            if (read)
                ret = nvme_read(zns_dev_ex->fd, zns_dev_ex->nsid, wp, lba_num, 0, 0, 0, 0, 0, io_num, (char *)(buffer) + ptr, 0, NULL);
            else
                ret = nvme_write(zns_dev_ex->fd, zns_dev_ex->nsid, wp, lba_num, 0, 0, 0, 0, 0, 0, io_num, (char *)(buffer) + ptr, 0, NULL);
            if (ret != 0)
                return ret;
            ptr += io_num;
            size_left -= io_num;
            wp += lba_num + 1;
        }
        return ret;
    }

    int metadata_write(struct zns_device_extra_info *info, void *buffer, uint32_t size)
    {
        __u64 res_lba;
        uint32_t blocks = size / zns_dev->lba_size_bytes;
        uint64_t last_zone = zns_dev->tparams.zns_num_zones - 1;
        if (size % zns_dev->lba_size_bytes)
        {
            printf("INVALID: write size not aligned to block size\n");
            return -1;
        }
        int ret;
        // if (zns_dev_ex->zone_states[last_zone] != EMPTY)
        // {
            ret = nvme_zns_mgmt_send(zns_dev_ex->fd, zns_dev_ex->nsid, last_zone * info->blocks_per_zone, false, NVME_ZNS_ZSA_RESET, 0, NULL);
            if (ret)
            {
                printf("ERROR: failed to reset at metadata block 0x%lx, ret: %ld \n", (last_zone)*info->blocks_per_zone, ret);
                return ret;
            }
        // }

        ret = nvme_zns_append(info->fd, info->nsid, last_zone * info->blocks_per_zone, blocks - 1, 0, 0, 0, 0, size, buffer, 0, NULL, &res_lba);
        if (ret)
        {
            printf("ERROR: failed to write at metadata block 0x%lx, ret: %ld, size:%d \n", (last_zone)*info->blocks_per_zone, ret, size);
            return ret;
        }
        return 0;
    }

    int metadata_read(struct zns_device_extra_info *info, void *buffer, uint32_t size)
    {
        uint32_t blocks = size / zns_dev->lba_size_bytes;
        if (size % zns_dev->lba_size_bytes)
        {
            printf("INVALID: read size not aligned to block size\n");
            return -1;
        }
        int ret = nvme_read(info->fd, info->nsid, (zns_dev->tparams.zns_num_zones - 1) * info->blocks_per_zone, blocks - 1, 0, 0, 0, 0, 0, size, buffer, 0, NULL);
        if (ret)
        {
            printf("INFO: failed to read metadata block at 0x%lx, ret: %ld\n", (zns_dev->tparams.zns_num_zones - 1) * info->blocks_per_zone, ret);
            return ret;
        }
        return 0;
    }

    int init_descriptor(struct zns_device_extra_info *info)
    {
        uint64_t lsb = zns_dev->lba_size_bytes;
        uint64_t bpz = zns_dev_ex->blocks_per_zone;
        char buffer[lsb * bpz] = {0};
        char size_char[lsb];

        if (zns_dev_ex->zone_states[zns_dev->tparams.zns_num_zones - 1] == EMPTY)
        {
            return 0;
        }

        metadata_read(info, size_char, lsb);
        uint32_t ptr = 0;
        uint32_t size = *(uint32_t *)(size_char);
        if (size == 0)
        {
            return 0;
        }

        metadata_read(info, buffer, roundup(size, lsb));

        info->log_zone_start = *(uint32_t *)(buffer + (ptr += sizeof(uint32_t)));
        info->log_zone_end = *(uint32_t *)(buffer + (ptr += sizeof(uint32_t)));
        info->data_zone_start = *(uint32_t *)(buffer + (ptr += sizeof(uint32_t)));
        info->data_zone_end = *(uint32_t *)(buffer + (ptr += sizeof(uint32_t)));

        ptr += sizeof(uint32_t);

        for (uint64_t i = info->log_zone_num_config; i < zns_dev->tparams.zns_num_zones - 1; i++, ptr += sizeof(uint8_t))
        {
            info->zone_states[i] = *(uint8_t *)(buffer + ptr);
        }

        uint32_t log_mapping_size = *(uint32_t *)(buffer + ptr);

        uint32_t data_mapping_size = *(uint32_t *)(buffer + (ptr += sizeof(int)));

        ptr += sizeof(uint32_t);

        for (int i = 0; i < log_mapping_size; i++, ptr += sizeof(int64_t))
        {
            int64_t key = *(uint64_t *)(buffer + ptr);
            int64_t value = *(uint64_t *)(buffer + (ptr += sizeof(int64_t)));
            log_mapping[key] = value;
        }

        for (int i = 0; i < data_mapping_size; i++, ptr += sizeof(int64_t))
        {
            int64_t key = *(uint64_t *)(buffer + ptr);
            int64_t value = *(uint64_t *)(buffer + (ptr += sizeof(int64_t)));
            data_mapping[key] = value;
        }

        return 0;
    }

    int restore_descriptor(struct zns_device_extra_info *info)
    {
        uint64_t bpz = zns_dev_ex->blocks_per_zone;
        uint64_t lsb = zns_dev->lba_size_bytes;
        char buffer[lsb * bpz] = {0};
        uint32_t ptr = 0;

        *(uint32_t *)(buffer + (ptr += sizeof(uint32_t))) = info->log_zone_start;
        *(uint32_t *)(buffer + (ptr += sizeof(uint32_t))) = info->log_zone_end;
        *(uint32_t *)(buffer + (ptr += sizeof(uint32_t))) = info->data_zone_start;
        *(uint32_t *)(buffer + (ptr += sizeof(uint32_t))) = info->data_zone_end;

        ptr += sizeof(uint32_t);

        for (uint64_t i = info->log_zone_num_config; i < zns_dev->tparams.zns_num_zones - 1; i++, ptr += sizeof(uint8_t))
        {
            *(uint8_t *)(buffer + ptr) = info->zone_states[i];
        }

        uint32_t log_mapping_size = log_mapping.size();
        *(uint32_t *)(buffer + ptr) = log_mapping_size;

        uint32_t data_mapping_size = data_mapping.size();
        *(uint32_t *)(buffer + (ptr += sizeof(uint32_t))) = data_mapping_size;
        ptr += sizeof(uint32_t);

        for (auto iter = log_mapping.begin(); iter != log_mapping.end(); iter++, ptr += sizeof(int64_t))
        {
            *(uint64_t *)(buffer + ptr) = iter->first;
            *(uint64_t *)(buffer + (ptr += sizeof(int64_t))) = iter->second;
        }

        for (auto iter = data_mapping.begin(); iter != data_mapping.end(); iter++, ptr += sizeof(int64_t))
        {
            *(uint64_t *)(buffer + ptr) = iter->first;
            *(uint64_t *)(buffer + (ptr += sizeof(int64_t))) = iter->second;
        }

        *(uint32_t *)(buffer) = ptr;

        metadata_write(info, buffer, roundup(ptr, lsb));

        return 0;
    }

    int get_free_lz_num(int offset)
    {
        return zns_dev_ex->log_zone_num_config - (zns_dev_ex->log_zone_end - zns_dev_ex->log_zone_start + offset) / zns_dev_ex->blocks_per_zone;
    }

    // find the next empty zone address
    int find_next_empty_zone()
    {
        for (uint64_t i = zns_dev_ex->log_zone_num_config; i < zns_dev->tparams.zns_num_zones - 1; i++)
        {
            if (zns_dev_ex->zone_states[i] == EMPTY)
            {
                return i * zns_dev_ex->blocks_per_zone;
            }
        }
        return -1;
    }

    int do_merge(std::unordered_map<int64_t, std::unordered_map<int64_t, int64_t> *> *zone_sets_ptr)
    {
        auto zone_set = *zone_sets_ptr;

        __u64 res_lba;
        int64_t ret, nlb = zns_dev_ex->blocks_per_zone, lsb = zns_dev->lba_size_bytes;
        char buffer[nlb * lsb] = {0};
        char log_buffer[lsb] = {0};

        auto iter = zone_set.begin();
        for (iter; iter != zone_set.end(); iter++)
        {
            int64_t zone_no = find_next_empty_zone(), old_zone = -1;
            bool used_log = false;
            if (zone_no == -1)
            {
                zone_no = (zns_dev_ex->log_zone_num_config - 1) * zns_dev_ex->blocks_per_zone; // use the last zone of log zones
                used_log = true;
            }

            if (data_mapping.find(iter->first) != data_mapping.end())
            {
                ret = ss_nvme_device_io_with_mdts(data_mapping[iter->first], buffer, nlb * lsb, true);
                if (ret)
                {
                    printf("ERROR: failed to read zone at 0x%lx, ret: %ld, during full merge\n", data_mapping[iter->first], ret);
                    return ret;
                }
                zns_dev_ex->zone_states[data_mapping[iter->first] / nlb] = EMPTY;
                old_zone = data_mapping[iter->first];
            }

            auto map = *(iter->second);
            auto ii = map.begin();
            for (ii; ii != map.end(); ii++)
            {
                ret = nvme_read(zns_dev_ex->fd, zns_dev_ex->nsid, ii->second, 0, 0, 0, 0, 0, 0, lsb, buffer + lsb * ii->first, 0, NULL);
                if (ret)
                {
                    printf("ERROR: failed to read log block at 0x%lx, ret: %ld\n", ii->second, ret);
                    return ret;
                }
            }

            if (used_log)
            {
                // means we didn't find a empty zone to write
                // So write to the last zone of log for backup
                // Reset the old zone and write to it
                // Reset the last zone of log
                nvme_zns_mgmt_send(zns_dev_ex->fd, zns_dev_ex->nsid, old_zone, false, NVME_ZNS_ZSA_RESET, 0, NULL);
                ret = ss_nvme_device_io_with_mdts(old_zone, buffer, nlb * lsb, false);
                if (ret)
                {
                    printf("ERROR: failed to write zone at 0x%lx, ret: %ld, used log zone\n", zone_no, ret);
                    return ret;
                }
                zns_dev_ex->zone_states[old_zone / nlb] = FULL;
                // nvme_zns_mgmt_send(zns_dev_ex->fd, zns_dev_ex->nsid, zone_no, false, NVME_ZNS_ZSA_RESET, 0, NULL);
            }
            else
            {
                ret = ss_nvme_device_io_with_mdts(zone_no, buffer, nlb * lsb, false);
                if (ret)
                {
                    printf("ERROR: failed to write zone at 0x%lx, ret: %ld\n", zone_no, ret);
                    return ret;
                }
                data_mapping[iter->first] = zone_no;
                zns_dev_ex->zone_states[zone_no / nlb] = FULL;

                if (old_zone != -1)
                    nvme_zns_mgmt_send(zns_dev_ex->fd, zns_dev_ex->nsid, old_zone, false, NVME_ZNS_ZSA_RESET, 0, NULL);
            }

            delete iter->second;
        }

        return 0;
    }

    void *gc_loop(void *args)
    {
        struct zns_device_extra_info *info = (struct zns_device_extra_info *)args;
        while (1)
        {
            pthread_mutex_lock(&info->gc_mutex);
            while (!info->gc_thread_stop && !info->do_gc)
            {
                pthread_cond_wait(&info->gc_wakeup, &info->gc_mutex);
            }

            if (info->gc_thread_stop)
            {
                pthread_mutex_unlock(&info->gc_mutex);
                break;
            }

            std::unordered_map<int64_t, std::unordered_map<int64_t, int64_t> *> zone_sets;
            std::unordered_map<int64_t, int64_t>::iterator iter;
            for (iter = log_mapping.begin(); iter != log_mapping.end(); iter++)
            {
                int64_t zone_no = address_2_zone(iter->first);
                if (!map_contains(zone_sets, zone_no))
                {
                    zone_sets[zone_no] = new std::unordered_map<int64_t, int64_t>;
                }
                auto map = zone_sets[zone_no];
                map->insert(std::pair<int64_t, int64_t>(address_2_offset(iter->first), iter->second));
                iter->second &= ENTRY_INVALID;
            }

            int ret = do_merge(&zone_sets);
            if (ret)
            {
                printf("Error: GC failed, ret:%d\n", ret);
            }

            for (int i = 0; i < info->log_zone_num_config; i++)
            {
                nvme_zns_mgmt_send(info->fd, info->nsid, i * info->blocks_per_zone, false, NVME_ZNS_ZSA_RESET, 0, NULL);
            }
            info->log_zone_end = info->log_zone_start;
            log_mapping.clear();

            info->do_gc = false;
            pthread_cond_signal(&info->gc_sleep);
            pthread_mutex_unlock(&info->gc_mutex);
        }

        return (void *)0;
    }

    int init_ss_zns_device(struct zdev_init_params *params, struct user_zns_device **my_dev)
    {
        int fd = nvme_open(params->name);
        if (fd < 0)
        {
            printf("device %s opening failed %d errno %d \n", params->name, fd, errno);
            return -fd;
        }

        struct zns_device_extra_info *info = static_cast<struct zns_device_extra_info *>(calloc(sizeof(struct zns_device_extra_info), 1));
        (*my_dev) = static_cast<struct user_zns_device *>(calloc(sizeof(struct user_zns_device), 1));
        info->fd = fd;
        info->gc_watermark = params->gc_wmark;
        info->log_zone_num_config = params->log_zones;
        (*my_dev)->_private = info;

        int ret = nvme_get_nsid(fd, &(info->nsid));
        if (ret != 0)
        {
            printf("ERROR: failed to retrieve the nsid %d \n", ret);
            return ret;
        }

        struct nvme_id_ns ns;
        ret = nvme_identify_ns(fd, info->nsid, &ns);
        if (ret)
        {
            printf("ERROR: failed to retrieve the nsid struct %d \n", ret);
            return ret;
        }

        if (params->force_reset)
        {
            ret = nvme_zns_mgmt_send(fd, info->nsid, 0, true, NVME_ZNS_ZSA_RESET, 0, NULL);
            if (ret)
            {
                printf("ERROR: failed to reset all zones %d \n", ret);
                return ret;
            }

            // info->data_zone_start = info->data_zone_end = params->log_zones * blocks_per_zone;
            info->log_zone_start = info->log_zone_end = 0;
        }

        (*my_dev)->lba_size_bytes = 1 << ns.lbaf[(ns.flbas & 0xf)].ds;
        (*my_dev)->tparams.zns_lba_size = (*my_dev)->lba_size_bytes;
        // info->mdts = get_mdts_size(info->fd, params->name);
        info->mdts = MDTS;

        struct nvme_zone_report report;
        ret = nvme_zns_mgmt_recv(fd, info->nsid, 0,
                                 NVME_ZNS_ZRA_REPORT_ZONES, NVME_ZNS_ZRAS_REPORT_ALL,
                                 0, sizeof(report), (void *)&report);
        if (ret != 0)
        {
            fprintf(stderr, "failed to report zones, ret %d \n", ret);
            return ret;
        }

        (*my_dev)->tparams.zns_num_zones = report.nr_zones;
        info->zone_states = (uint8_t *)calloc(report.nr_zones, sizeof(uint8_t));

        uint64_t total_size = sizeof(report) + (report.nr_zones * sizeof(struct nvme_zns_desc));
        char *zone_reports = (char *)calloc(1, total_size);
        // dont need to report all for milestone 2, but needed for milestone 5
        ret = nvme_zns_mgmt_recv(fd, info->nsid, 0,
                                 NVME_ZNS_ZRA_REPORT_ZONES, NVME_ZNS_ZRAS_REPORT_ALL,
                                 1, total_size, (void *)zone_reports);
        if (ret)
        {
            free(zone_reports);
            printf("ERROR: failed to get zone reports %d \n", ret);
            return ret;
        }

        uint64_t blocks_per_zone = ((struct nvme_zone_report *)zone_reports)->entries[0].zcap;
        info->blocks_per_zone = blocks_per_zone;
        (*my_dev)->tparams.zns_zone_capacity = blocks_per_zone * (*my_dev)->lba_size_bytes;
        // need to update this when doing persistence
        (*my_dev)->capacity_bytes = (report.nr_zones - params->log_zones - 1) * ((*my_dev)->tparams.zns_zone_capacity);

        for (uint64_t i = params->log_zones; i < report.nr_zones; i++)
        {
            info->zone_states[i] = (((struct nvme_zone_report *)zone_reports)->entries[i].zs >> 4);
        }

        free(zone_reports);

        // populate log_mapping for ms5
        // populate data_mapping for ms5

        // record log_zone_start and log_zone_end for ms5
        // record data_zone_start and data_zone_end for ms5

        ret = pthread_create(&info->gc_thread_id, NULL, &gc_loop, info);
        if (ret)
        {
            printf("ERROR: failed to create gc thread %d \n", ret);
            return ret;
        }

        zns_dev = *my_dev;
        zns_dev_ex = info;

        // read log_mapping data_mapping zns_device_extra_info
        // if log zone number < 512, one zone reserve for metadata_zone is enough
        ret = init_descriptor(info);

        return 0;
    }

    int zns_udevice_read(struct user_zns_device *my_dev, uint64_t address, void *buffer, uint32_t size)
    {
        if (size % my_dev->lba_size_bytes)
        {
            printf("INVALID: read size not aligned to block size\n");
            return -1;
        }

        int32_t ret, lba_s = my_dev->lba_size_bytes;
        uint32_t blocks = size / lba_s, num_read = 0;
        struct zns_device_extra_info *info = (struct zns_device_extra_info *)my_dev->_private;
        for (uint64_t i = address; i < address + blocks * lba_s; i += lba_s)
        {
            uint64_t entry;
            bool read_data = true;
            // the top bit 1 means invalid
            if (map_contains(log_mapping, i))
            {
                entry = log_mapping[i];
                read_data = (entry & ENTRY_INVALID);
            }

            if (read_data)
            {
                uint64_t zone_no = address_2_zone(i);
                if (!map_contains(data_mapping, zone_no))
                {
                    // nothing at this address
                    memset(buffer + num_read, 0, lba_s);
                    num_read += lba_s;
                    continue;
                }

                entry = data_mapping[zone_no] + address_2_offset(i);
            }

            ret = nvme_read(info->fd, info->nsid, (entry & ~ENTRY_INVALID), 0, 0, 0, 0, 0, 0, lba_s, (char *)buffer + num_read, 0, NULL);
            if (ret)
            {
                printf("ERROR: failed to read at 0x%lx, ret: %d\n", (entry & ~ENTRY_INVALID), ret);
                return ret;
            }
            num_read += lba_s;
        }

        return 0;
    }

    int zns_udevice_write(struct user_zns_device *my_dev, uint64_t address, void *buffer, uint32_t size)
    {
        if (size % my_dev->lba_size_bytes)
        {
            printf("INVALID: write size not aligned to block size\n");
            return -1;
        }

        struct zns_device_extra_info *info = (struct zns_device_extra_info *)my_dev->_private;
        uint32_t blocks = size / my_dev->lba_size_bytes;
        pthread_mutex_lock(&zns_dev_ex->gc_mutex);
        while (get_free_lz_num(blocks) <= info->gc_watermark)
        {
            zns_dev_ex->do_gc = true;
            pthread_cond_signal(&zns_dev_ex->gc_wakeup);
            pthread_cond_wait(&zns_dev_ex->gc_sleep, &zns_dev_ex->gc_mutex);
        }

        __u64 res_lba;
        int32_t ret, lz_end_before = info->log_zone_end, z_no = info->log_zone_end / info->blocks_per_zone;
        ret = nvme_zns_append(info->fd, info->nsid, z_no * info->blocks_per_zone, blocks - 1, 0, 0, 0, 0, size, buffer, 0, NULL, &res_lba);
        if (ret)
        {
            printf("ERROR: failed to write at 0x%d, ret: %d \n", info->log_zone_end, ret);
            return ret;
        }

        info->log_zone_end = res_lba + 1;
        for (uint32_t i = 0; i < blocks; i++)
        {
            log_mapping[address + i * my_dev->lba_size_bytes] = lz_end_before + i;
        }

        pthread_mutex_unlock(&zns_dev_ex->gc_mutex);
        return 0;
    }

    int deinit_ss_zns_device(struct user_zns_device *my_dev)
    {
        struct zns_device_extra_info *info = (struct zns_device_extra_info *)my_dev->_private;
        info->gc_thread_stop = true;
        pthread_cond_signal(&info->gc_wakeup);

        // wait for gc stop
        pthread_join(info->gc_thread_id, NULL);

        pthread_mutex_destroy(&info->gc_mutex);
        pthread_cond_destroy(&info->gc_wakeup);

        int ret = restore_descriptor(info);

        free(info->zone_states);
        free(my_dev->_private);
        free(my_dev);
        return 0;
    }
}
