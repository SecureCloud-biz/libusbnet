/***************************************************************************
*   Copyright (C) 2009 Marek Vavrusa <marek@vavrusa.com>                  *
*                                                                         *
*   This program is free software; you can redistribute it and/or modify  *
*   it under the terms of the GNU Library General Public License as       *
*   published by the Free Software Foundation; either version 2 of the    *
*   License, or (at your option) any later version.                       *
*                                                                         *
*   This program is distributed in the hope that it will be useful,       *
*   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
*   GNU General Public License for more details.                          *
*                                                                         *
*   You should have received a copy of the GNU Library General Public     *
*   License along with this program; if not, write to the                 *
*   Free Software Foundation, Inc.,                                       *
*   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.         *
***************************************************************************/
/*! \file usbnet.c
    \brief Reimplementation of libusb prototypes.
    \author Marek Vavrusa <marek@vavrusa.com>
    \addtogroup libusbnet
    @{
  */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/shm.h>
#include <netinet/in.h>
#include "usbnet.h"
#include "protocol.h"

/** Imported from libusb-0.1/descriptors.c
 */
static void usb_destroy_configuration(struct usb_device *dev);

/* Call serialization.
 */
static pthread_mutex_t __mutex = PTHREAD_MUTEX_INITIALIZER;
static void call_lock() {
   pthread_mutex_lock(&__mutex);
}

static void call_release() {
   pthread_mutex_unlock(&__mutex);
}

//! Remote socket filedescriptor
static int __remote_fd = -1;

//! Remote USB busses with devices
static struct usb_bus* __remote_bus = 0;
extern struct usb_bus* usb_busses;

/* Global variables manipulators.
 */

/** Free allocated busses and virtual devices.
  * Function is called on executable exit (atexit()).
  */
void deinit_hostfd() {
   debug_msg("freeing busses ...");

   // Unhook global variable
   usb_busses = 0;

   // Free busses
   struct usb_bus* cur = NULL;
   while(__remote_bus != NULL) {

      // Shift bus ptr
      cur = __remote_bus;
      __remote_bus = cur->next;

      // Free bus devices
      struct usb_device* dev = NULL;
      while(cur->devices != NULL) {
         dev = cur->devices;
         cur->devices = dev->next;

         // Destroy configuration and free device
         usb_destroy_configuration(dev);
         if(dev->children != NULL)
            free(dev->children);
         free(dev);
      }

      // Free bus
      free(cur);
   }
}

/** Return host socket descriptor.
  * Retrieve socket descriptor from SHM if not present
  * and hook exit function for cleanup.
  */
int init_hostfd() {

   // Check exit function
   static char exitf_hooked = 0;
   if(!exitf_hooked) {
      atexit(&deinit_hostfd);
      exitf_hooked = 1;
   }

   // Check fd
   if(__remote_fd == -1) {

      // Get fd from SHM
      int shm_id = 0;
      printf("IPC: accessing segment at key 0x%x (%d bytes)\n", SHM_KEY, SHM_SIZE);
      if((shm_id = shmget(SHM_KEY, SHM_SIZE, 0666)) != -1) {

         // Attach segment and read fd
         printf("IPC: attaching segment %d\n", shm_id);
         void* shm_addr = NULL;
         if ((shm_addr = shmat(shm_id, NULL, 0)) != (void *) -1) {

            // Read fd
            __remote_fd = *((int*) shm_addr);

            // Detach
            shmdt(shm_addr);
         }
      }

      // Check resulting fd
      printf("IPC: remote fd is %d\n", __remote_fd);
   }

   // Check peer name - keep-alive
   struct sockaddr_in addr;
   int len = sizeof(addr);
   if(getpeername(__remote_fd, (struct sockaddr*)& addr, &len) < 0)
      __remote_fd = -1;

   if(__remote_fd == -1) {
      fprintf(stderr, "IPC: unable to access remote fd\n");
      exit(1);
   }

   return __remote_fd;
}

/* libusb functions reimplementation.
 * \see http://libusb.sourceforge.net/doc/functions.html
 */

/* libusb(1):
 * Core functions.
 */

/** Initialize USB subsystem. */
void usb_init(void)
{
   // Initialize remote fd
   call_lock();
   int fd = init_hostfd();

   // Create buffer
   char buf[PACKET_MINSIZE];
   Packet pkt = pkt_create(buf, PACKET_MINSIZE);
   pkt_init(&pkt, UsbInit);
   pkt_send(&pkt, fd);
   call_release();

   // Initialize locally
   debug_msg("called");
}

/** Find busses on remote host. */
int usb_find_busses(void)
{
  // Get remote fd
   call_lock();
   int fd = init_hostfd();

   // Create buffer
   char buf[32];
   Packet pkt = pkt_create(buf, 32);
   pkt_init(&pkt, UsbFindBusses);
   pkt_send(&pkt, fd);

   // Get number of changes
   int res = 0;
   Iterator it;
   if(pkt_recv(fd, &pkt) > 0) {
      if(pkt_begin(&pkt, &it) != NULL) {
         if(it.type == IntegerType)
            res = as_uint(it.val, it.len);
      }
   }

   // Return remote result
   call_release();
   debug_msg("returned %d", res);
   return res;
}

/** Find devices on remote host.
  * Create new devices on local virtual bus.
  * \warning Function replaces global usb_busses variable from libusb.
  */
int usb_find_devices(void)
{
   // Get remote fd
   call_lock();
   int fd = init_hostfd();

   // Create buffer
   char buf[4096];
   Packet pkt = pkt_create(buf, 4096);
   pkt_init(&pkt, UsbFindDevices);
   pkt_send(&pkt, fd);

   // Get number of changes
   int res = 0;
   Iterator it;
   if(pkt_recv(fd, &pkt) > 0) {
      pkt_begin(&pkt, &it);

      // Get return value
      if(it.type == IntegerType) {
         res = as_uint(it.val, it.len);
      }

      // Allocate virtualbus
      struct usb_bus vbus;
      vbus.next = __remote_bus;
      struct usb_bus* rbus = &vbus;

      // Get busses
      while(iter_next(&it) != NULL) {

         // Evaluate
         if(it.type == StructureType) {
            iter_enter(&it);

            // Allocate bus
            if(rbus->next == NULL) {

               // Allocate next item
               struct usb_bus* nbus = malloc(sizeof(struct usb_bus));
               memset(nbus, 0, sizeof(struct usb_bus));
               rbus->next = nbus;
               nbus->prev = rbus;
               rbus = nbus;
            }
            else
               rbus = rbus->next;

            // Read dirname
            if(it.type == OctetType) {
               strcpy(rbus->dirname, as_string(it.val, it.len));
               iter_next(&it);
            }

            // Read location
            if(it.type == IntegerType) {
               rbus->location = as_int(it.val, it.len);
               iter_next(&it);
            }

            // Read devices
            struct usb_device vdev;
            vdev.next = rbus->devices;
            struct usb_device* dev = &vdev;
            while(it.type == SequenceType) {
               iter_enter(&it);

               // Initialize
               if(dev->next == NULL) {
                  dev->next = malloc(sizeof(struct usb_device));
                  memset(dev->next, 0, sizeof(struct usb_device));
                  dev->next->bus = rbus;
                  if(dev != &vdev)
                     dev->next->prev = dev;
                  if(rbus->devices == NULL)
                     rbus->devices = dev->next;
               }

               dev = dev->next;

               // Read filename
               if(it.type == OctetType) {
                  strcpy(dev->filename, as_string(it.val, it.len));
                  iter_next(&it);
               }

               // Read description
               if(it.type == RawType) {
                  memcpy(&dev->descriptor, it.val, it.len);
                  iter_next(&it);
               }

               // Read config
               if(it.type == RawType) {

                  // Ensure struct under/overlap
                  int szlen = sizeof(struct usb_config_descriptor);
                  if(szlen > it.len)
                     szlen = it.len;

                  dev->config = malloc(sizeof(struct usb_config_descriptor));
                  memcpy(dev->config, it.val, szlen);

                  // Allocate interfaces
                  dev->config->interface = NULL;
                  if(dev->config->bNumInterfaces > 0) {
                     dev->config->interface = malloc(dev->config->bNumInterfaces * sizeof(struct usb_interface));
                  }

                  //! \todo Implement usb_device extra interfaces - are they needed?
                  dev->config->extralen = 0;
                  dev->config->extra = NULL;

                  iter_next(&it);
               }

               // Load interfaces
               unsigned i, j, k;
               for(i = 0; i < dev->config->bNumInterfaces; ++i) {
                  struct usb_interface* iface = &dev->config->interface[i];

                  // Read altsettings count
                  iface->num_altsetting = as_int(it.val, it.len);
                  iter_next(&it);

                  // Allocate altsettings
                  if(iface->num_altsetting > 0) {
                     iface->altsetting = malloc(iface->num_altsetting * sizeof(struct usb_interface_descriptor));
                  }

                  // Load altsettings
                  for(j = 0; j < iface->num_altsetting; ++j) {

                     // Ensure struct under/overlap
                     struct usb_interface_descriptor* altsetting = &iface->altsetting[j];
                     int szlen = sizeof(struct usb_interface_descriptor);
                     if(szlen > it.len)
                        szlen = it.len;

                     memcpy(altsetting, it.val, szlen);
                     iter_next(&it);

                     // Allocate endpoints
                     if(altsetting->bNumEndpoints > 0) {
                        altsetting->endpoint = malloc(altsetting->bNumEndpoints * sizeof(struct usb_endpoint_descriptor));
                     }

                     // Load endpoints
                     for(k = 0; k < altsetting->bNumEndpoints; ++k) {
                        struct usb_endpoint_descriptor* endpoint = &altsetting->endpoint[k];
                        int szlen = sizeof(struct usb_endpoint_descriptor);
                        if(szlen > it.len)
                           szlen = it.len;

                        memcpy(endpoint, it.val, szlen);
                        iter_next(&it);

                        // Null extra descriptors.
                        endpoint->extralen = 0;
                        endpoint->extra = NULL;
                     }

                     // Null extra interfaces.
                     altsetting->extralen = 0;
                     altsetting->extra = NULL;
                  }
               }

               // Read devnum
               if(it.type == IntegerType) {
                  dev->devnum = as_int(it.val, it.len);
                  iter_next(&it);
               }

              log_msg("Bus %s Device %s: ID %04x:%04x", rbus->dirname, dev->filename, dev->descriptor.idVendor, dev->descriptor.idProduct);
            }

            // Free unused devices
            while(dev->next != NULL) {
               struct usb_device* ddev = dev->next;
               debug_msg("deleting device %03d", ddev->devnum);
               dev->next = ddev->next;
               free(ddev);
            }

         }
         else {
            debug_msg("unexpected item identifier 0x%02x", it.type);
            iter_next(&it);
         }
      }

      // Deallocate unnecessary busses
      while(rbus->next != NULL) {
         debug_msg("deleting bus %03d", rbus->next->location);
         struct usb_bus* bus = rbus->next;
         rbus->next = bus->next;
      }

      // Save busses
      __remote_bus = vbus.next;

      // Override original variable
      debug_msg("overriding global usb_busses on %p", usb_busses);
      usb_busses = __remote_bus;
   }

   // Return remote result
   call_release();
   debug_msg("returned %d", res);
   return res;
}

/** Return pointer to virtual bus list.
  */
struct usb_bus* usb_get_busses(void)
{
   //! \todo Merge both local - attached busses in future?
   debug_msg("returned %p", __remote_bus);
   return __remote_bus;
}

/* libusb(2):
 * Device operations.
 */

usb_dev_handle *usb_open(struct usb_device *dev)
{
   // Get remote fd
   call_lock();
   int fd = init_hostfd();

   // Send packet
   char buf[255];
   Packet pkt = pkt_create(buf, 255);
   pkt_init(&pkt, UsbOpen);
   pkt_append(&pkt, IntegerType, sizeof(dev->bus->location), &dev->bus->location);
   pkt_append(&pkt, IntegerType, sizeof(dev->devnum),        &dev->devnum);
   pkt_send(&pkt, fd);

   // Get response
   int res = -1, devfd = -1;
   if(pkt_recv(fd, &pkt) > 0 && pkt.buf[0] == UsbOpen) {
      Iterator it;
      pkt_begin(&pkt, &it);
      if(it.type == IntegerType) {
         res = as_int(it.val, it.len);
         iter_next(&it);
      }
      if(it.type == IntegerType) {
          devfd = as_int(it.val, it.len);
      }
   }

   // Evaluate
   usb_dev_handle* udev = NULL;
   if(res >= 0) {
      udev = malloc(sizeof(usb_dev_handle));
      udev->fd = devfd;
      udev->device = dev;
      udev->bus = dev->bus;
      udev->config = udev->interface = udev->altsetting = -1;
   }

   call_release();
   debug_msg("returned %d (fd %d)", res, devfd);
   return udev;
}

int usb_close(usb_dev_handle *dev)
{
   // Get remote fd
   call_lock();
   int fd = init_hostfd();

   // Send packet
   char buf[255];
   Packet pkt = pkt_create(buf, 255);
   pkt_init(&pkt, UsbClose);
   pkt_append(&pkt, IntegerType, sizeof(dev->fd),  &dev->fd);
   pkt_send(&pkt, fd);

   // Free device
   free(dev);

   // Get response
   int res = -1;
   if(pkt_recv(fd, &pkt) > 0 && pkt.buf[0] == UsbClose) {
      Iterator it;
      pkt_begin(&pkt, &it);
      if(it.type == IntegerType) {
         res = as_int(it.val, it.len);
      }
   }

   call_release();
   debug_msg("returned %d", res);
   return res;
}

int usb_set_configuration(usb_dev_handle *dev, int configuration)
{
   // Get remote fd
   call_lock();
   int fd = init_hostfd();

   // Prepare packet
   char buf[255];
   Packet pkt = pkt_create(buf, 255);
   pkt_init(&pkt, UsbSetConfiguration);
   pkt_append(&pkt, IntegerType, sizeof(dev->fd), &dev->fd);
   pkt_append(&pkt, IntegerType, sizeof(int), &configuration);
   pkt_send(&pkt, fd);

   // Get response
   int res = -1;
   if(pkt_recv(fd, &pkt) > 0 && pkt.buf[0] == UsbSetConfiguration) {
      Iterator it;
      pkt_begin(&pkt, &it);

      // Read result
      if(it.type == IntegerType) {
         res = as_int(it.val, it.len);
         iter_next(&it);
      }

      // Read callback configuration
      if(it.type == IntegerType) {
         configuration = as_int(it.val, it.len);
      }
   }

   // Save configuration
   dev->config = configuration;

   // Return response
   call_release();
   debug_msg("returned %d", res);
   return res;
}

int usb_set_altinterface(usb_dev_handle *dev, int alternate)
{
   // Get remote fd
   call_lock();
   int fd = init_hostfd();

   // Prepare packet
   char buf[255];
   Packet pkt = pkt_create(buf, 255);
   pkt_init(&pkt, UsbSetAltInterface);
   pkt_append(&pkt, IntegerType, sizeof(dev->fd), &dev->fd);
   pkt_append(&pkt, IntegerType, sizeof(int), &alternate);
   pkt_send(&pkt, fd);

   // Get response
   int res = -1;
   if(pkt_recv(fd, &pkt) > 0 && pkt.buf[0] == UsbSetAltInterface) {
      Iterator it;
      pkt_begin(&pkt, &it);

      // Read result
      if(it.type == IntegerType) {
         res = as_int(it.val, it.len);
         iter_next(&it);
      }

      // Read callback configuration
      if(it.type == IntegerType) {
         alternate = as_int(it.val, it.len);
      }
   }

   // Save configuration
   dev->altsetting = alternate;

   // Return response
   call_release();
   debug_msg("returned %d", res);
   return res;
}

int usb_resetep(usb_dev_handle *dev, unsigned int ep)
{
   // Get remote fd
   call_lock();
   int fd = init_hostfd();

   // Prepare packet
   char buf[255];
   Packet pkt = pkt_create(buf, 255);
   pkt_init(&pkt, UsbResetEp);
   pkt_append(&pkt, IntegerType, sizeof(dev->fd), &dev->fd);
   pkt_append(&pkt, IntegerType, sizeof(int), &ep);
   pkt_send(&pkt, fd);

   // Get response
   int res = -1;
   if(pkt_recv(fd, &pkt) > 0 && pkt.buf[0] == UsbResetEp) {
      Iterator it;
      pkt_begin(&pkt, &it);

      // Read result
      if(it.type == IntegerType) {
         res = as_int(it.val, it.len);
      }
   }

   // Return response
   call_release();
   debug_msg("returned %d", res);
   return res;
}

int usb_clear_halt(usb_dev_handle *dev, unsigned int ep)
{
   // Get remote fd
   call_lock();
   int fd = init_hostfd();

   // Prepare packet
   char buf[255];
   Packet pkt = pkt_create(buf, 255);
   pkt_init(&pkt, UsbClearHalt);
   pkt_append(&pkt, IntegerType, sizeof(dev->fd), &dev->fd);
   pkt_append(&pkt, IntegerType, sizeof(int), &ep);
   pkt_send(&pkt, fd);

   // Get response
   int res = -1;
   if(pkt_recv(fd, &pkt) > 0 && pkt.buf[0] == UsbClearHalt) {
      Iterator it;
      pkt_begin(&pkt, &it);

      // Read result
      if(it.type == IntegerType) {
         res = as_int(it.val, it.len);
      }
   }

   // Return response
   call_release();
   debug_msg("returned %d", res);
   return res;
}

int usb_reset(usb_dev_handle *dev)
{
   // Get remote fd
   call_lock();
   int fd = init_hostfd();

   // Prepare packet
   char buf[255];
   Packet pkt = pkt_create(buf, 255);
   pkt_init(&pkt, UsbReset);
   pkt_append(&pkt, IntegerType, sizeof(dev->fd), &dev->fd);
   pkt_send(&pkt, fd);

   // Get response
   int res = -1;
   if(pkt_recv(fd, &pkt) > 0 && pkt.buf[0] == UsbReset) {
      Iterator it;
      pkt_begin(&pkt, &it);

      // Read result
      if(it.type == IntegerType) {
         res = as_int(it.val, it.len);
      }
   }

   // Return response
   call_release();
   debug_msg("returned %d", res);
   return res;
}

int usb_claim_interface(usb_dev_handle *dev, int interface)
{
   // Get remote fd
   call_lock();
   int fd = init_hostfd();

   // Send packet
   char buf[255];
   Packet pkt = pkt_create(buf, 255);
   pkt_init(&pkt, UsbClaimInterface);
   pkt_append(&pkt, IntegerType, sizeof(dev->fd),  &dev->fd);
   pkt_append(&pkt, IntegerType, sizeof(int),      &interface);
   pkt_send(&pkt, fd);

   // Get response
   int res = -1;
   if(pkt_recv(fd, &pkt) > 0 && pkt.buf[0] == UsbClaimInterface) {
      Iterator it;
      pkt_begin(&pkt, &it);
      if(it.type == IntegerType) {
         res = as_int(it.val, it.len);
      }
   }

   call_release();
   printf("%s: returned %d\n", __func__, res);
   return res;
}

int usb_release_interface(usb_dev_handle *dev, int interface)
{
   // Get remote fd
   call_lock();
   int fd = init_hostfd();

   // Send packet
   char buf[255];
   Packet pkt = pkt_create(buf, 255);
   pkt_init(&pkt, UsbReleaseInterface);
   pkt_append(&pkt, IntegerType, sizeof(dev->fd),  &dev->fd);
   pkt_append(&pkt, IntegerType, sizeof(int),      &interface);
   pkt_send(&pkt, fd);

   // Get response
   int res = -1;
   if(pkt_recv(fd, &pkt) > 0 && pkt.buf[0] == UsbReleaseInterface) {
      Iterator it;
      pkt_begin(&pkt, &it);
      if(it.type == IntegerType) {
         res = as_int(it.val, it.len);
      }
   }

   call_release();
   debug_msg("returned %d", res);
   return res;
}

/* libusb(3):
 * Control transfers.
 */

int usb_control_msg(usb_dev_handle *dev, int requesttype, int request,
        int value, int index, char *bytes, int size, int timeout)
{
   // Get remote fd
   call_lock();
   int fd = init_hostfd();

   // Prepare packet
   Packet* pkt = pkt_new(size + 128, UsbControlMsg);
   pkt_append(pkt, IntegerType, sizeof(dev->fd), &dev->fd);
   pkt_append(pkt, IntegerType, sizeof(int), &requesttype);
   pkt_append(pkt, IntegerType, sizeof(int), &request);
   pkt_append(pkt, IntegerType, sizeof(int), &value);
   pkt_append(pkt, IntegerType, sizeof(int), &index);
   pkt_append(pkt, OctetType,   size,        bytes);
   pkt_append(pkt, IntegerType, sizeof(int), &timeout);
   pkt_send(pkt, fd);

   // Get response
   int res = -1;
   if(pkt_recv(fd, pkt) > 0 && pkt->buf[0] == UsbControlMsg) {
      Iterator it;
      pkt_begin(pkt, &it);
      if(it.type == IntegerType) {
         res = as_int(it.val, it.len);
         iter_next(&it);
      }
      if(it.type == OctetType) {
         if(res > 0) {
            int minlen = (res > size) ? size : res;
            memcpy(bytes, it.val, minlen);
         }
      }
   }

   // Return response
   pkt_free(pkt);
   call_release();
   debug_msg("returned %d", res);
   return res;
}

/* libusb(4):
 * Bulk transfers.
 */

int usb_bulk_read(usb_dev_handle *dev, int ep, char *bytes, int size, int timeout)
{
   // Get remote fd
   call_lock();
   int fd = init_hostfd();

   // Prepare packet
   Packet* pkt = pkt_new(size + 128, UsbBulkRead);
   pkt_append(pkt, IntegerType, sizeof(dev->fd), &dev->fd);
   pkt_append(pkt, IntegerType, sizeof(int), &ep);
   pkt_append(pkt, IntegerType, sizeof(int), &size);
   pkt_append(pkt, IntegerType, sizeof(int), &timeout);
   pkt_send(pkt, fd);

   // Get response
   int res = -1;
   unsigned char op = 0x00;
   if(pkt_recv(fd, pkt) > 0) {
      op = pkt->buf[0];
      Iterator it;
      pkt_begin(pkt, &it);
      if(it.type == IntegerType) {
         res = as_int(it.val, it.len);
         iter_next(&it);
      }
      if(it.type == OctetType) {
         if(res > 0) {
            int minlen = (res > size) ? size : res;
            memcpy(bytes, it.val, minlen);
         }
      }
   }

   // Return response
   pkt_free(pkt);
   call_release();
   debug_msg("returned %d", res);
   return res;
}

int usb_bulk_write(usb_dev_handle *dev, int ep, const char *bytes, int size, int timeout)
{
   // Get remote fd
   call_lock();
   int fd = init_hostfd();

   // Prepare packet
   Packet* pkt = pkt_new(size + 128, UsbBulkWrite);
   pkt_append(pkt, IntegerType, sizeof(dev->fd), &dev->fd);
   pkt_append(pkt, IntegerType, sizeof(int), &ep);
   pkt_append(pkt, OctetType,   size,        bytes);
   pkt_append(pkt, IntegerType, sizeof(int), &timeout);
   pkt_send(pkt, fd);

   // Get response
   int res = -1;
   if(pkt_recv(fd, pkt) > 0 && pkt->buf[0] == UsbBulkWrite) {
      Iterator it;
      pkt_begin(pkt, &it);
      if(it.type == IntegerType) {
         res = as_int(it.val, it.len);
         iter_next(&it);
      }
   }

   // Return response
   pkt_free(pkt);
   call_release();
   debug_msg("returned %d", res);
   return res;
}

/* libusb(5):
 * Interrupt transfers.
 */
int usb_interrupt_write(usb_dev_handle *dev, int ep, const char *bytes, int size, int timeout)
{
   // Get remote fd
   call_lock();
   int fd = init_hostfd();

   // Prepare packet
   Packet* pkt = pkt_new(size + 128, UsbInterruptWrite);
   pkt_append(pkt, IntegerType, sizeof(dev->fd), &dev->fd);
   pkt_append(pkt, IntegerType, sizeof(int), &ep);
   pkt_append(pkt, OctetType,   size,        bytes);
   pkt_append(pkt, IntegerType, sizeof(int), &timeout);
   pkt_send(pkt, fd);

   // Get response
   int res = -1;
   if(pkt_recv(fd, pkt) > 0 && pkt->buf[0] == UsbInterruptWrite) {
      Iterator it;
      pkt_begin(pkt, &it);
      if(it.type == IntegerType) {
         res = as_int(it.val, it.len);
         iter_next(&it);
      }
   }

   // Return response
   pkt_free(pkt);
   call_release();
   debug_msg("returned %d", res);
   return res;
}

int usb_interrupt_read(usb_dev_handle *dev, int ep, char *bytes, int size, int timeout)
{
   // Get remote fd
   call_lock();
   int fd = init_hostfd();

   // Prepare packet
   Packet* pkt = pkt_new(size + 128, UsbInterruptRead);
   pkt_append(pkt, IntegerType, sizeof(dev->fd), &dev->fd);
   pkt_append(pkt, IntegerType, sizeof(int), &ep);
   pkt_append(pkt, IntegerType, sizeof(int), &size);
   pkt_append(pkt, IntegerType, sizeof(int), &timeout);
   pkt_send(pkt, fd);

   // Get response
   int res = -1;
   unsigned char op = 0x00;
   if(pkt_recv(fd, pkt) > 0 && pkt->buf[0] == UsbInterruptRead) {
      op = pkt->buf[0];
      Iterator it;
      pkt_begin(pkt, &it);
      if(it.type == IntegerType) {
         res = as_int(it.val, it.len);
         iter_next(&it);
      }
      if(it.type == OctetType) {
         if(res > 0) {
            int minlen = (res > size) ? size : res;
            memcpy(bytes, it.val, minlen);
         }
      }
   }

   // Return response
   pkt_free(pkt);
   call_release();
   debug_msg("returned %d", res);
   return res;
}

/* libusb(6):
 * Non-portable.
 */

int usb_detach_kernel_driver_np(usb_dev_handle *dev, int interface)
{
   // Get remote fd
   call_lock();
   int fd = init_hostfd();

   // Send packet
   char buf[255];
   Packet pkt = pkt_create(buf, 255);
   pkt_init(&pkt, UsbDetachKernelDriver);
   pkt_append(&pkt, IntegerType, sizeof(dev->fd),  &dev->fd);
   pkt_append(&pkt, IntegerType, sizeof(int),      &interface);
   pkt_send(&pkt, fd);

   // Get response
   int res = -1;
   if(pkt_recv(fd, &pkt) > 0 && pkt.buf[0] == UsbDetachKernelDriver) {
      Iterator it;
      pkt_begin(&pkt, &it);
      if(it.type == IntegerType) {
         res = as_int(it.val, it.len);
      }
   }

   call_release();
   debug_msg("returned %d", res);
   return res;

}

/* Imported from libusb-0.1.12 for forward compatibility with libusb-1.0.
 * This overrides libusb-0.1 as well as libusb-1.0 calls.
 * @see libusb-0.1.12/usb.c:219
 * @see libusb-0.1.12/usb.c:230
 */
int usb_get_string(usb_dev_handle *dev, int index, int langid, char *buf,
        size_t buflen)
{
  /*
   * We can't use usb_get_descriptor() because it's lacking the index
   * parameter. This will be fixed in libusb 1.0
   */
  return usb_control_msg(dev, USB_ENDPOINT_IN, USB_REQ_GET_DESCRIPTOR,
                        (USB_DT_STRING << 8) + index, langid, buf, buflen, 1000);
}
int usb_get_string_simple(usb_dev_handle *dev, int index, char *buf, size_t buflen)
{
  char tbuf[255];	/* Some devices choke on size > 255 */
  int ret, langid, si, di;

  /*
   * Asking for the zero'th index is special - it returns a string
   * descriptor that contains all the language IDs supported by the
   * device. Typically there aren't many - often only one. The
   * language IDs are 16 bit numbers, and they start at the third byte
   * in the descriptor. See USB 2.0 specification, section 9.6.7, for
   * more information on this. */
  ret = usb_get_string(dev, 0, 0, tbuf, sizeof(tbuf));
  if (ret < 0)
    return ret;

  if (ret < 4)
    return -EIO;

  langid = tbuf[2] | (tbuf[3] << 8);

  ret = usb_get_string(dev, index, langid, tbuf, sizeof(tbuf));
  if (ret < 0)
    return ret;

  if (tbuf[1] != USB_DT_STRING)
    return -EIO;

  if (tbuf[0] > ret)
    return -EFBIG;

  for (di = 0, si = 2; si < tbuf[0]; si += 2) {
    if (di >= (buflen - 1))
      break;

    if (tbuf[si + 1])	/* high byte */
      buf[di++] = '?';
    else
      buf[di++] = tbuf[si];
  }

  buf[di] = 0;

  return di;
}

static void usb_destroy_configuration(struct usb_device *dev)
{
  int c, i, j, k;

  if (!dev->config)
    return;

  for (c = 0; c < dev->descriptor.bNumConfigurations; c++) {
    struct usb_config_descriptor *cf = &dev->config[c];

    if (!cf->interface)
      continue;

    for (i = 0; i < cf->bNumInterfaces; i++) {
      struct usb_interface *ifp = &cf->interface[i];

      if (!ifp->altsetting)
        continue;

      for (j = 0; j < ifp->num_altsetting; j++) {
        struct usb_interface_descriptor *as = &ifp->altsetting[j];

        if (as->extra)
          free(as->extra);

        if (!as->endpoint)
          continue;

        for (k = 0; k < as->bNumEndpoints; k++) {
          if (as->endpoint[k].extra)
            free(as->endpoint[k].extra);
        }
        free(as->endpoint);
      }

      free(ifp->altsetting);
    }

    free(cf->interface);
  }

  free(dev->config);
}
/** @} */
