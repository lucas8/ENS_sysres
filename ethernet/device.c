
#include "device.h"
#include "ethernet.h"
#include "ports.h"
#include "proto.h"
#include "logging.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <pthread.h>
#include <hurd.h>
#include <hurd/io.h>
#include <device/device.h>

/****************************************************************
 *********************** File Device ****************************
 ****************************************************************/
struct file_device_main_data {
    device_t dev;
    mach_port_t in;
    mach_port_t sel; /* Sel is used for select queries on fd */
    mach_port_t set; /* Contains sel and in */
    mach_port_t out;
};

void* file_device_main(void* data) {
    char buffer[4096];
    int size, written;
    struct file_device_main_data* params = data;
    typeinfo_t tpinfo;
    mach_port_t used;
    mach_msg_header_t* hd;
    mach_msg_type_t* tp;
    struct net_rcv_msg* net_msg;

    while(1) {
        used = params->set;
        if(!receive_data_low(&used, &hd, buffer, 4096)) continue;

        switch(hd->msgh_id) {
            case lvl1_frame:
                net_msg       = (struct net_rcv_msg*)hd;
                tpinfo.size   = net_msg->packet_type.msgt_size * net_msg->packet_type.msgt_number;
                tpinfo.number = 1;
                tpinfo.id     = lvl3_frame;
                send_data(params->out, &tpinfo, net_msg->packet);
                break;

            case lvl2_frame:
                tp = (mach_msg_type_t*)((char*)hd + sizeof(mach_msg_header_t));
                size = tp->msgt_size * tp->msgt_number;
                device_write(params->dev, D_NOWAIT, 0, buffer, size, &written);
                break;

            default:
                log_variadic("Device thread received invalid type id : %d\n", hd->msgh_id);
                break;
        }
    }
}

static struct bpf_insn bpf_filter[] =
{
    {NETF_IN|NETF_BPF, 0, 0, 0},           /* Header. */
    {BPF_LD|BPF_H|BPF_ABS, 0, 0, 12},      /* Load Ethernet type */
    {BPF_JMP|BPF_JEQ|BPF_K, 2, 0, 0x0806}, /* Accept ARP */
    {BPF_JMP|BPF_JEQ|BPF_K, 1, 0, 0x0800}, /* Accept IPv4 */
    {BPF_JMP|BPF_JEQ|BPF_K, 0, 1, 0x86DD}, /* Accept IPv6 */
    {BPF_RET|BPF_K, 0, 0, 1500},           /* And return 1500 bytes */
    {BPF_RET|BPF_K, 0, 0, 0},              /* Or discard it all */
};
static int bpf_filter_len = sizeof (bpf_filter) / sizeof (short);

ethernet_error_t open_file_device(struct device* dev, const char* path) {
    struct file_device_main_data* data;
    kern_return_t ret;
    int pret;
    ethernet_error_t err;
    file_t fd;
    int address[2];
    size_t count;

    data = malloc(sizeof(struct file_device_main_data));
    if(!data) {
        err = ETH_AGAIN;
        goto err;
    }

    fd        = MACH_PORT_NULL;
    data->dev = MACH_PORT_NULL;
    data->in  = MACH_PORT_NULL;
    data->sel = MACH_PORT_NULL;
    data->set = MACH_PORT_NULL;
    data->out = MACH_PORT_NULL;
    err = ETH_IO;

    /* Open file */
    fd = file_name_lookup(path, O_READ, 0);
    if(fd == MACH_PORT_NULL) goto err;

    /* Get device instance */
    ret = device_open(fd, D_READ | D_WRITE, "", &data->dev);
    if(ret != KERN_SUCCESS) goto err;

    /* Get ports for communication with main thread */
    ret = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &data->in);
    if(ret != KERN_SUCCESS) goto err;
    ret = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &data->out);
    if(ret != KERN_SUCCESS) goto err;
    ret = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &data->sel);
    if(ret != KERN_SUCCESS) goto err;

    /* Setup device */
    ret = device_set_filter(data->dev, data->sel, MACH_MSG_TYPE_MAKE_SEND, 0,
            (unsigned short*)bpf_filter, bpf_filter_len);
    if(ret != KERN_SUCCESS) goto err;

    /* Port set to receive from data->in and dev */
    ret = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &data->set);
    if(ret != KERN_SUCCESS) goto err;
    err = mach_port_move_member(mach_task_self(), data->sel, data->set);
    if(ret != KERN_SUCCESS) goto err;
    err = mach_port_move_member(mach_task_self(), data->in, data->set);
    if(ret != KERN_SUCCESS) goto err;


    dev->out = data->in;
    dev->in  = data->out;

    /* Get MAC address */
    count = 2;
    ret = device_get_status(data->dev, NET_ADDRESS, address, &count);
    if(ret != KERN_SUCCESS) goto err;
    dev->mac.bytes[0] =  address[0]        % 256;
    dev->mac.bytes[1] = (address[0] >>  8) % 256;
    dev->mac.bytes[2] = (address[0] >> 16) % 256;
    dev->mac.bytes[3] = (address[0] >> 24) % 256;
    dev->mac.bytes[4] =  address[1]        % 256;
    dev->mac.bytes[5] = (address[1] >>  8) % 256;

    /* Launch the thread */
    pret = pthread_create(&dev->thread, NULL, file_device_main, (void*)data);
    if(pret) {
        err = ETH_AGAIN;
        goto err;
    }

    return ETH_SUCCESS;

err:
    if(data) {
        if(fd        != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), fd);
        if(data->dev != MACH_PORT_NULL) device_close(data->dev);
        if(data->in  != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), data->in);
        if(data->out != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), data->out);
        if(data->set != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), data->sel);
        if(data->set != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), data->set);
        free(data);
    }
    return err;
}

