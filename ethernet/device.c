
#include "device.h"
#include "ethernet.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <fcntl.h>
#include <pthread.h>

/****************************************************************
 *********************** File Device ****************************
 ****************************************************************/
ethernet_error_t file_write(int fd, void* data, size_t* size) {
    ssize_t written;

    written = write(fd, data, *size);
    if(written == -1) {
        *size = 0;
        switch(errno) {
            case EAGAIN:
            case EINTR:
                return ETH_AGAIN;
            case EIO:
            case EFBIG:
            case ENOSPC:
            case EPIPE:
                return ETH_IO;
            case EBADF:
            case EFAULT:
            case EINVAL:
            default:
                return ETH_INVALID;
        }
    }

    *size = written;
    return ETH_SUCCESS;
}

ethernet_error_t file_read(int fd, void* data, size_t* size) {
    ssize_t rd;

    rd = read(fd, data, *size);
    if(rd == -1) {
        *size = 0;
        switch(errno) {
            case EAGAIN:
            case EINTR:
                return ETH_AGAIN;
            case EIO:
                return ETH_IO;
            case EBADF:
            case EISDIR:
            case EFAULT:
            case EINVAL:
            default:
                return ETH_INVALID;
        }
    }

    *size = rd;
    return ETH_SUCCESS;
}

ethernet_error_t file_close(int fd) {
    close(fd);
    return ETH_SUCCESS;
}

struct file_device_main_data {
    int fd;
    mach_port_t in;
    mach_port_t out;
};

void* file_device_main(void* data) {
    char buffer[4096];
    size_t size;
    struct file_device_main_data* params = data;
    fd_set in, out;

    while(1) {
        FD_SET(params->fd, &in);
        FD_SET(params->fd, &out);
        select(params->fd, &in, &out, NULL, NULL);

        if(FD_ISSET(params->fd, &in)) {
            size = read(params->fd, buffer, 4096);
            /* TODO send frame to params->out */
        }

        if(FD_ISSET(params->fd, &out)) {
            /* TODO read from params->in and write it to params->fd */
        }
    }
}

ethernet_error_t open_file_device(struct device* dev, const char* path) {
    struct file_device_main_data* data;
    kern_return_t ret;
    int pret;

    data = malloc(sizeof(struct file_device_main_data));
    if(!data) return ETH_AGAIN;

    data->fd = open(path, O_RDWR | O_APPEND);
    if(data->fd == -1) {
        switch(errno) {
            case EWOULDBLOCK:
                return ETH_AGAIN;
            case ELOOP:
            case EMFILE:
            case ENFILE:
            case ENODEV:
            case ENOMEM:
            case ENOSPC:
            case ENXIO:
            case EOVERFLOW:
                return ETH_IO;
            case EROFS:
            case ETXTBSY:
            case EPERM:
            case ENOTDIR:
            case ENOENT:
            case ENAMETOOLONG:
            case EACCES:
            case EEXIST:
            case EFAULT:
            case EISDIR:
            default:
                return ETH_INVALID;
        }
    }

    ret = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &data->in);
    if(ret != KERN_SUCCESS) {
        close(data->fd);
        free(data);
        return ETH_IO;
    }

    ret = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &data->out);
    if(ret != KERN_SUCCESS) {
        close(data->fd);
        mach_port_deallocate(mach_task_self(), data->in);
        free(data);
        return ETH_IO;
    }

    dev->out = data->in;
    dev->in  = data->out;
    /* TODO find MAC address */

    pret = pthread_create(&dev->thread, NULL, file_device_main, (void*)data);
    if(pret) {
        close(data->fd);
        mach_port_deallocate(mach_task_self(), data->in);
        mach_port_deallocate(mach_task_self(), data->out);
        free(data);
        return ETH_AGAIN;
    }


    return ETH_SUCCESS;
}

/****************************************************************
 *********************** Mach Device ****************************
 ****************************************************************/
ethernet_error_t open_mach_device(struct device* dev, const char* name) {
    /* TODO */
}

/****************************************************************
 *********************** Dummy Device ***************************
 ****************************************************************/
ethernet_error_t dummy_write(void* dev, void* data, size_t* size) {
    printf("Sending message of size %u\n", *size);
    return ETH_SUCCESS;
}

static char buffer[2048];
ethernet_error_t dummy_read(void* dev, void* data, size_t* size) {
    struct eth_frame fr;

    printf("Source: ");
    scanf("%s", buffer);
    read_mac_address(buffer, &fr.src);
    printf("Destination: ");
    scanf("%s", buffer);
    read_mac_address(buffer, &fr.dst);

    printf("Ethertype: ");
    scanf("%hX", &fr.ethertype);
    printf("Data: ");
    scanf("%s", buffer);

    fr.size = strlen(buffer);
    *size   = fr.size;
    fr.data = buffer;
    return make_frame(&fr, data, *size);
}

ethernet_error_t dummy_close(void* dev) {
    return ETH_SUCCESS;
}

ethernet_error_t open_dummy_device(struct device* dev) {
    /*
    dev->dev   = NULL;
    dev->write = dummy_write;
    dev->read  = dummy_read;
    dev->close = dummy_close;
    */
    return ETH_SUCCESS;
}

