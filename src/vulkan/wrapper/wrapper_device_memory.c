#define native_handle_t __native_handle_t
#define buffer_handle_t __buffer_handle_t
#include "wrapper_private.h"
#include "wrapper_entrypoints.h"
#include "vk_common_entrypoints.h"
#undef native_handle_t
#undef buffer_handle_t
#include <sys/stat.h>
#include "util/hash_table.h"
#include "util/os_file.h"
#include "vk_util.h"
#include <dlfcn.h>

#include <android/hardware_buffer.h>
#include <vndk/hardware_buffer.h>
#include <sys/mman.h>

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_AllocateMemory(VkDevice _device,
                       const VkMemoryAllocateInfo* pAllocateInfo,
                       const VkAllocationCallbacks* pAllocator,
                       VkDeviceMemory* pMemory)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);

   const VkImportAndroidHardwareBufferInfoANDROID *import_ahb_info;
   const VkImportMemoryFdInfoKHR *import_fd_info;
   const VkExportMemoryAllocateInfo *export_info;
   VkExportMemoryAllocateInfo local_export_info;
   VkMemoryAllocateInfo wrapper_allocate_info;
   struct wrapper_device_memory *memory;
   VkMemoryPropertyFlags mem_flags;
   bool can_get_ahardware_buffer;
   bool can_get_dmabuf_fd;
   VkResult result;

   wrapper_allocate_info.sType = pAllocateInfo->sType;
   wrapper_allocate_info.pNext = pAllocateInfo->pNext;
   wrapper_allocate_info.allocationSize = pAllocateInfo->allocationSize;
   wrapper_allocate_info.memoryTypeIndex = pAllocateInfo->memoryTypeIndex;

   mem_flags = device->physical->memory_properties.memoryTypes
      [pAllocateInfo->memoryTypeIndex].propertyFlags;

   if (!device->vk.enabled_extensions.EXT_map_memory_placed ||
      (mem_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0)
   {  
      result = device->dispatch_table.AllocateMemory(device->dispatch_handle,
                                                   &wrapper_allocate_info,
                                                   pAllocator,
                                                   pMemory);

      if (result != VK_SUCCESS)
         return vk_error(device, result);
      else  
         return result;
   }
  
   memory = vk_zalloc2(&device->vk.alloc, pAllocator, sizeof(*memory),
                       8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!memory)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   memory->alloc_size = pAllocateInfo->allocationSize;
   
   memory->dmabuf_fd = -1;
   memory->ahardware_buffer = NULL;

   import_ahb_info = vk_find_struct_const(pAllocateInfo,
      IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID);
   import_fd_info = vk_find_struct_const(pAllocateInfo,
      IMPORT_MEMORY_FD_INFO_KHR);
   export_info = vk_find_struct_const(pAllocateInfo,
      EXPORT_MEMORY_ALLOCATE_INFO);

   if (import_ahb_info) {
      memory->ahardware_buffer = import_ahb_info->buffer;
      AHardwareBuffer_acquire(memory->ahardware_buffer);
   } else if (import_fd_info) {
      memory->dmabuf_fd = os_dupfd_cloexec(import_fd_info->fd);
   } else if (export_info == NULL) {
      local_export_info = (VkExportMemoryAllocateInfo) {
         .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
         .pNext = wrapper_allocate_info.pNext,
         .handleTypes = device->physical->vk.supported_extensions.
            EXT_external_memory_dma_buf ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT : 
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID
      };
      wrapper_allocate_info.pNext = &local_export_info;
      export_info = &local_export_info;
   }
   
   result = device->dispatch_table.AllocateMemory(device->dispatch_handle,
                                                  &wrapper_allocate_info,
                                                  pAllocator,
                                                  pMemory);
                                                  
   if (result != VK_SUCCESS) {
      if (memory->ahardware_buffer)
         AHardwareBuffer_release(memory->ahardware_buffer);
      if (memory->dmabuf_fd != -1)
         close(memory->dmabuf_fd);
      vk_free2(&device->vk.alloc, pAllocator, memory);
      return vk_error(device, result);
   }
   
   can_get_dmabuf_fd = (export_info && export_info->handleTypes ==
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
   can_get_ahardware_buffer = (export_info && export_info->handleTypes ==
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID);

   if (can_get_dmabuf_fd) {
      const VkMemoryGetFdInfoKHR get_fd_info = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
         .memory = *pMemory,
         .handleType =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      };
      device->dispatch_table.GetMemoryFdKHR(device->dispatch_handle,
         &get_fd_info, &memory->dmabuf_fd);
   } else if (can_get_ahardware_buffer) {
      const VkMemoryGetAndroidHardwareBufferInfoANDROID get_ahb_info = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_GET_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
         .memory = *pMemory,
      };
      device->dispatch_table.GetMemoryAndroidHardwareBufferANDROID(
         device->dispatch_handle, &get_ahb_info, &memory->ahardware_buffer);
   }

   _mesa_hash_table_insert(device->memorys, (void *)(*pMemory), memory);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
wrapper_FreeMemory(VkDevice _device, VkDeviceMemory _memory,
                   const VkAllocationCallbacks* pAllocator)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   struct hash_entry *entry = NULL;

   if (_memory != VK_NULL_HANDLE)
      entry = _mesa_hash_table_search(device->memorys, (void *)_memory);

   if (entry) {
      struct wrapper_device_memory *memory = entry->data;
      if (memory->map_address && memory->map_size)
         munmap(memory->map_address, memory->map_size);
      if (memory->ahardware_buffer)
         AHardwareBuffer_release(memory->ahardware_buffer);
      if (memory->dmabuf_fd != -1)
         close(memory->dmabuf_fd);
      vk_free2(&device->vk.alloc, pAllocator, memory);
      _mesa_hash_table_remove(device->memorys, entry);
   }

   device->dispatch_table.FreeMemory(device->dispatch_handle,
                                     _memory,
                                     pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_MapMemory2KHR(VkDevice _device,
                      const VkMemoryMapInfoKHR* pMemoryMapInfo,
                      void** ppData)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   struct wrapper_device_memory *memory;
   const VkMemoryMapPlacedInfoEXT *placed_info;
   const struct hash_entry *entry = NULL;
   int fd;

   placed_info = vk_find_struct_const(pMemoryMapInfo->pNext,
                                      MEMORY_MAP_PLACED_INFO_EXT);
   if (pMemoryMapInfo->memory != VK_NULL_HANDLE)
      entry = _mesa_hash_table_search(device->memorys,
                                      (void *)pMemoryMapInfo->memory);
   if (!placed_info || !entry) {
     
      return device->dispatch_table.MapMemory(device->dispatch_handle,
                                              pMemoryMapInfo->memory,
                                              pMemoryMapInfo->offset,
                                              pMemoryMapInfo->size,
                                              0,
                                              ppData);
   }
   memory = entry->data;

   if (memory->map_address) {
      if (placed_info->pPlacedAddress != memory->map_address) {
         return VK_ERROR_MEMORY_MAP_FAILED;
      } else {
         *ppData = (char *)memory->map_address + pMemoryMapInfo->offset;
         return VK_SUCCESS;
      }
   }
   assert(memory->dmabuf_fd >= 0 || memory->ahardware_buffer != NULL);

   if (memory->ahardware_buffer) {
      const native_handle_t *handle;
      const int *handle_fds;

      handle = AHardwareBuffer_getNativeHandle(memory->ahardware_buffer);
      handle_fds = &handle->data[0];

      int idx;
      for (idx = 0; idx < handle->numFds; idx++) {
         size_t size = lseek(handle_fds[idx], 0, SEEK_END);
         if (size >= memory->alloc_size) {
            break;
         }
      }
      assert(idx < handle->numFds);
      fd = handle_fds[idx];
   } else {
      fd = memory->dmabuf_fd;
   }

   if (pMemoryMapInfo->size == VK_WHOLE_SIZE)
      memory->map_size = memory->alloc_size > 0 ?
         memory->alloc_size : lseek(fd, 0, SEEK_END);
   else
      memory->map_size = pMemoryMapInfo->size;

   memory->map_address = mmap(placed_info->pPlacedAddress,
                              memory->map_size,
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_FIXED,
                              fd, 0);
                              
   if (memory->map_address == MAP_FAILED) {
      memory->map_address = NULL;
      memory->map_size = 0;
      fprintf(stderr, "%s: mmap failed\n", __func__);
      return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);
   }

   *ppData = (char *)memory->map_address + pMemoryMapInfo->offset;

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
wrapper_UnmapMemory(VkDevice _device, VkDeviceMemory _memory) {
   vk_common_UnmapMemory(_device, _memory);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_UnmapMemory2KHR(VkDevice _device,
                        const VkMemoryUnmapInfoKHR* pMemoryUnmapInfo)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   const struct hash_entry *entry = NULL;

   if (pMemoryUnmapInfo->memory != VK_NULL_HANDLE)
      entry = _mesa_hash_table_search(device->memorys,
                                      (void *)pMemoryUnmapInfo->memory);
   if (entry) {
      struct wrapper_device_memory *memory = entry->data;
      if (pMemoryUnmapInfo->flags & VK_MEMORY_UNMAP_RESERVE_BIT_EXT) {
         memory->map_address = mmap(memory->map_address, memory->map_size,
            PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
         if (memory->map_address == MAP_FAILED) {
            fprintf(stderr, "Failed to replace mapping with reserved memory");
            return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);
         }
      } else {
         munmap(memory->map_address, memory->map_size);
      }

      memory->map_size = 0;
      memory->map_address = NULL;
   }

   device->dispatch_table.UnmapMemory(device->dispatch_handle,
                                      pMemoryUnmapInfo->memory);
   return VK_SUCCESS;
}
