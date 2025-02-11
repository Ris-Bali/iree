
// Copyright 2024 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "experimental/hip/stream_command_buffer.h"

#include "experimental/hip/hip_buffer.h"
#include "experimental/hip/native_executable.h"
#include "experimental/hip/pipeline_layout.h"
#include "experimental/hip/status_util.h"
#include "iree/hal/utils/resource_set.h"

typedef struct iree_hal_hip_stream_command_buffer_t {
  iree_hal_command_buffer_t base;
  iree_allocator_t host_allocator;

  const iree_hal_hip_dynamic_symbols_t* hip_symbols;

  hipStream_t hip_stream;

  // A resource set to maintain references to all resources used within the
  // command buffer. Reset on each begin.
  iree_hal_resource_set_t* resource_set;

  // Staging arena used for host->device transfers.
  // Used for when we need HIP to be able to reference memory as it performs
  // asynchronous operations.
  iree_arena_allocator_t arena;

  int32_t push_constants[IREE_HAL_HIP_MAX_PUSH_CONSTANT_COUNT];

  // The current bound descriptor sets.
  struct {
    hipDeviceptr_t bindings[IREE_HAL_HIP_MAX_DESCRIPTOR_SET_BINDING_COUNT];
  } descriptor_sets[IREE_HAL_HIP_MAX_DESCRIPTOR_SET_COUNT];
} iree_hal_hip_stream_command_buffer_t;

static const iree_hal_command_buffer_vtable_t
    iree_hal_hip_stream_command_buffer_vtable;

static iree_hal_hip_stream_command_buffer_t*
iree_hal_hip_stream_command_buffer_cast(iree_hal_command_buffer_t* base_value) {
  IREE_HAL_ASSERT_TYPE(base_value, &iree_hal_hip_stream_command_buffer_vtable);
  return (iree_hal_hip_stream_command_buffer_t*)base_value;
}

iree_status_t iree_hal_hip_stream_command_buffer_create(
    iree_hal_device_t* device,
    const iree_hal_hip_dynamic_symbols_t* hip_symbols,
    iree_hal_command_buffer_mode_t mode,
    iree_hal_command_category_t command_categories,
    iree_host_size_t binding_capacity, hipStream_t stream,
    iree_arena_block_pool_t* block_pool, iree_allocator_t host_allocator,
    iree_hal_command_buffer_t** out_command_buffer) {
  IREE_ASSERT_ARGUMENT(device);
  IREE_ASSERT_ARGUMENT(hip_symbols);
  IREE_ASSERT_ARGUMENT(out_command_buffer);
  *out_command_buffer = NULL;

  if (binding_capacity > 0) {
    // TODO(#10144): support indirect command buffers with binding tables.
    return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                            "indirect command buffers not yet implemented");
  }

  IREE_TRACE_ZONE_BEGIN(z0);

  iree_hal_hip_stream_command_buffer_t* command_buffer = NULL;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_allocator_malloc(host_allocator, sizeof(*command_buffer),
                                (void**)&command_buffer));

  iree_hal_command_buffer_initialize(
      device, mode, command_categories, IREE_HAL_QUEUE_AFFINITY_ANY,
      binding_capacity, &iree_hal_hip_stream_command_buffer_vtable,
      &command_buffer->base);
  command_buffer->host_allocator = host_allocator;
  command_buffer->hip_symbols = hip_symbols;
  command_buffer->hip_stream = stream;
  iree_arena_initialize(block_pool, &command_buffer->arena);

  iree_status_t status =
      iree_hal_resource_set_allocate(block_pool, &command_buffer->resource_set);

  *out_command_buffer = &command_buffer->base;
  IREE_TRACE_ZONE_END(z0);
  return status;
}

static void iree_hal_hip_stream_command_buffer_destroy(
    iree_hal_command_buffer_t* base_command_buffer) {
  iree_hal_hip_stream_command_buffer_t* command_buffer =
      iree_hal_hip_stream_command_buffer_cast(base_command_buffer);
  iree_allocator_t host_allocator = command_buffer->host_allocator;
  IREE_TRACE_ZONE_BEGIN(z0);

  iree_hal_resource_set_free(command_buffer->resource_set);
  iree_arena_deinitialize(&command_buffer->arena);
  iree_allocator_free(host_allocator, command_buffer);

  IREE_TRACE_ZONE_END(z0);
}

bool iree_hal_hip_stream_command_buffer_isa(
    iree_hal_command_buffer_t* command_buffer) {
  return iree_hal_resource_is(&command_buffer->resource,
                              &iree_hal_hip_stream_command_buffer_vtable);
}

static iree_status_t iree_hal_hip_stream_command_buffer_begin(
    iree_hal_command_buffer_t* base_command_buffer) {
  return iree_ok_status();
}

static iree_status_t iree_hal_hip_stream_command_buffer_end(
    iree_hal_command_buffer_t* base_command_buffer) {
  iree_hal_hip_stream_command_buffer_t* command_buffer =
      iree_hal_hip_stream_command_buffer_cast(base_command_buffer);
  IREE_TRACE_ZONE_BEGIN(z0);

  // Reset the arena as there should be nothing using it now that we've
  // dispatched all our operations inline.
  // NOTE: the resource set may contain resources we need to drop as we don't
  //       need to keep them live any longer than it takes to schedule the
  //       operations. In a real command buffer we would be this stream command
  //       buffer is strictly used to perform inline execution/replay of
  //       deferred command buffers that are retaining the resources already.
  iree_arena_reset(&command_buffer->arena);
  iree_hal_resource_set_free(command_buffer->resource_set);
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_hal_resource_set_allocate(command_buffer->arena.block_pool,
                                         &command_buffer->resource_set));

  IREE_TRACE_ZONE_END(z0);
  return iree_ok_status();
}

static void iree_hal_hip_stream_command_buffer_begin_debug_group(
    iree_hal_command_buffer_t* base_command_buffer, iree_string_view_t label,
    iree_hal_label_color_t label_color,
    const iree_hal_label_location_t* location) {}

static void iree_hal_hip_stream_command_buffer_end_debug_group(
    iree_hal_command_buffer_t* base_command_buffer) {}

static iree_status_t iree_hal_hip_stream_command_buffer_execution_barrier(
    iree_hal_command_buffer_t* base_command_buffer,
    iree_hal_execution_stage_t source_stage_mask,
    iree_hal_execution_stage_t target_stage_mask,
    iree_hal_execution_barrier_flags_t flags,
    iree_host_size_t memory_barrier_count,
    const iree_hal_memory_barrier_t* memory_barriers,
    iree_host_size_t buffer_barrier_count,
    const iree_hal_buffer_barrier_t* buffer_barriers) {
  if (iree_any_bit_set(source_stage_mask, IREE_HAL_EXECUTION_STAGE_HOST) ||
      iree_any_bit_set(target_stage_mask, IREE_HAL_EXECUTION_STAGE_HOST)) {
    return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                            "barrier involving host not yet supported");
  }

  if (flags != IREE_HAL_EXECUTION_BARRIER_FLAG_NONE) {
    return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                            "non-zero barrier flag not yet supported");
  }
  IREE_TRACE_ZONE_BEGIN(z0);

  // Nothing to do for barriers between memory operations or dispatches--HIP
  // stream semantics guarantees execution and memory visibility in program
  // order.

  IREE_TRACE_ZONE_END(z0);
  return iree_ok_status();
}

static iree_status_t iree_hal_hip_stream_command_buffer_signal_event(
    iree_hal_command_buffer_t* base_command_buffer, iree_hal_event_t* event,
    iree_hal_execution_stage_t source_stage_mask) {
  return iree_make_status(IREE_STATUS_UNIMPLEMENTED, "event not yet supported");
}

static iree_status_t iree_hal_hip_stream_command_buffer_reset_event(
    iree_hal_command_buffer_t* base_command_buffer, iree_hal_event_t* event,
    iree_hal_execution_stage_t source_stage_mask) {
  return iree_make_status(IREE_STATUS_UNIMPLEMENTED, "event not yet supported");
}

static iree_status_t iree_hal_hip_stream_command_buffer_wait_events(
    iree_hal_command_buffer_t* base_command_buffer,
    iree_host_size_t event_count, const iree_hal_event_t** events,
    iree_hal_execution_stage_t source_stage_mask,
    iree_hal_execution_stage_t target_stage_mask,
    iree_host_size_t memory_barrier_count,
    const iree_hal_memory_barrier_t* memory_barriers,
    iree_host_size_t buffer_barrier_count,
    const iree_hal_buffer_barrier_t* buffer_barriers) {
  return iree_make_status(IREE_STATUS_UNIMPLEMENTED, "event not yet supported");
}

static iree_status_t iree_hal_hip_stream_command_buffer_discard_buffer(
    iree_hal_command_buffer_t* base_command_buffer, iree_hal_buffer_t* buffer) {
  // We could mark the memory as invalidated so that if managed HIP does not
  // try to copy it back to the host.
  return iree_ok_status();
}

static iree_status_t iree_hal_hip_stream_command_buffer_fill_buffer(
    iree_hal_command_buffer_t* base_command_buffer,
    iree_hal_buffer_t* target_buffer, iree_device_size_t target_offset,
    iree_device_size_t length, const void* pattern,
    iree_host_size_t pattern_length) {
  iree_hal_hip_stream_command_buffer_t* command_buffer =
      iree_hal_hip_stream_command_buffer_cast(base_command_buffer);
  IREE_TRACE_ZONE_BEGIN(z0);

  hipDeviceptr_t target_device_buffer = iree_hal_hip_buffer_device_pointer(
      iree_hal_buffer_allocated_buffer(target_buffer));
  target_offset += iree_hal_buffer_byte_offset(target_buffer);
  hipDeviceptr_t dst = (uint8_t*)target_device_buffer + target_offset;
  size_t num_elements = length / pattern_length;

  switch (pattern_length) {
    case 4: {
      IREE_HIP_RETURN_AND_END_ZONE_IF_ERROR(
          z0, command_buffer->hip_symbols,
          hipMemsetD32Async(dst, *(const uint32_t*)(pattern), num_elements,
                            command_buffer->hip_stream),
          "hipMemsetD32Async");
      break;
    }
    case 2: {
      IREE_HIP_RETURN_AND_END_ZONE_IF_ERROR(
          z0, command_buffer->hip_symbols,
          hipMemsetD16Async(dst, *(const uint16_t*)(pattern), num_elements,
                            command_buffer->hip_stream),
          "hipMemsetD16Async");
      break;
    }
    case 1: {
      IREE_HIP_RETURN_AND_END_ZONE_IF_ERROR(
          z0, command_buffer->hip_symbols,
          hipMemsetD8Async(dst, *(const uint8_t*)(pattern), num_elements,
                           command_buffer->hip_stream),
          "hipMemsetD8Async");
      break;
    }
    default:
      IREE_TRACE_ZONE_END(z0);
      return iree_make_status(IREE_STATUS_INTERNAL,
                              "unsupported fill pattern length");
  }

  IREE_TRACE_ZONE_END(z0);
  return iree_ok_status();
}

static iree_status_t iree_hal_hip_stream_command_buffer_update_buffer(
    iree_hal_command_buffer_t* base_command_buffer, const void* source_buffer,
    iree_host_size_t source_offset, iree_hal_buffer_t* target_buffer,
    iree_device_size_t target_offset, iree_device_size_t length) {
  iree_hal_hip_stream_command_buffer_t* command_buffer =
      iree_hal_hip_stream_command_buffer_cast(base_command_buffer);
  IREE_TRACE_ZONE_BEGIN(z0);

  // Allocate scratch space in the arena for the data and copy it in.
  // The update buffer API requires that the command buffer capture the host
  // memory at the time the method is called in case the caller wants to reuse
  // the memory. Because HIP memcpys are async if we didn't copy it's possible
  // for the reused memory to change before the stream reaches the copy
  // operation and get the wrong data.
  const uint8_t* src = (const uint8_t*)source_buffer + source_offset;
  if (command_buffer->arena.block_pool) {
    uint8_t* storage = NULL;
    IREE_RETURN_AND_END_ZONE_IF_ERROR(
        z0,
        iree_arena_allocate(&command_buffer->arena, length, (void**)&storage));
    memcpy(storage, src, length);
    src = storage;
  }

  // Issue the copy using the scratch memory as the source.
  hipDeviceptr_t target_device_buffer = iree_hal_hip_buffer_device_pointer(
      iree_hal_buffer_allocated_buffer(target_buffer));
  hipDeviceptr_t dst = (uint8_t*)target_device_buffer +
                       iree_hal_buffer_byte_offset(target_buffer) +
                       target_offset;
  IREE_HIP_RETURN_AND_END_ZONE_IF_ERROR(
      z0, command_buffer->hip_symbols,
      hipMemcpyHtoDAsync(dst, (void*)src, length, command_buffer->hip_stream),
      "hipMemcpyHtoDAsync");

  IREE_TRACE_ZONE_END(z0);
  return iree_ok_status();
}

static iree_status_t iree_hal_hip_stream_command_buffer_copy_buffer(
    iree_hal_command_buffer_t* base_command_buffer,
    iree_hal_buffer_t* source_buffer, iree_device_size_t source_offset,
    iree_hal_buffer_t* target_buffer, iree_device_size_t target_offset,
    iree_device_size_t length) {
  iree_hal_hip_stream_command_buffer_t* command_buffer =
      iree_hal_hip_stream_command_buffer_cast(base_command_buffer);
  IREE_TRACE_ZONE_BEGIN(z0);

  hipDeviceptr_t target_device_buffer = iree_hal_hip_buffer_device_pointer(
      iree_hal_buffer_allocated_buffer(target_buffer));
  target_offset += iree_hal_buffer_byte_offset(target_buffer);
  hipDeviceptr_t source_device_buffer = iree_hal_hip_buffer_device_pointer(
      iree_hal_buffer_allocated_buffer(source_buffer));
  source_offset += iree_hal_buffer_byte_offset(source_buffer);
  hipDeviceptr_t dst = (uint8_t*)target_device_buffer + target_offset;
  hipDeviceptr_t src = (uint8_t*)source_device_buffer + source_offset;

  IREE_HIP_RETURN_AND_END_ZONE_IF_ERROR(
      z0, command_buffer->hip_symbols,
      hipMemcpyAsync(dst, src, length, hipMemcpyDeviceToDevice,
                     command_buffer->hip_stream),
      "hipMemcpyAsync");

  IREE_TRACE_ZONE_END(z0);
  return iree_ok_status();
}

static iree_status_t iree_hal_hip_stream_command_buffer_collective(
    iree_hal_command_buffer_t* base_command_buffer, iree_hal_channel_t* channel,
    iree_hal_collective_op_t op, uint32_t param,
    iree_hal_buffer_binding_t send_binding,
    iree_hal_buffer_binding_t recv_binding, iree_device_size_t element_count) {
  return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                          "collectives not yet supported");
}

static iree_status_t iree_hal_hip_stream_command_buffer_push_constants(
    iree_hal_command_buffer_t* base_command_buffer,
    iree_hal_pipeline_layout_t* pipeline_layout, iree_host_size_t offset,
    const void* values, iree_host_size_t values_length) {
  iree_hal_hip_stream_command_buffer_t* command_buffer =
      iree_hal_hip_stream_command_buffer_cast(base_command_buffer);
  IREE_TRACE_ZONE_BEGIN(z0);

  iree_host_size_t constant_base_index = offset / sizeof(int32_t);
  for (iree_host_size_t i = 0; i < values_length / sizeof(int32_t); i++) {
    command_buffer->push_constants[i + constant_base_index] =
        ((uint32_t*)values)[i];
  }

  IREE_TRACE_ZONE_END(z0);
  return iree_ok_status();
}

static iree_status_t iree_hal_hip_stream_command_buffer_push_descriptor_set(
    iree_hal_command_buffer_t* base_command_buffer,
    iree_hal_pipeline_layout_t* pipeline_layout, uint32_t set,
    iree_host_size_t binding_count,
    const iree_hal_descriptor_set_binding_t* bindings) {
  if (binding_count > IREE_HAL_HIP_MAX_DESCRIPTOR_SET_BINDING_COUNT) {
    return iree_make_status(
        IREE_STATUS_RESOURCE_EXHAUSTED,
        "exceeded available binding slots for push "
        "descriptor set #%" PRIu32 "; requested %" PRIhsz " vs. maximal %d",
        set, binding_count, IREE_HAL_HIP_MAX_DESCRIPTOR_SET_BINDING_COUNT);
  }

  iree_hal_hip_stream_command_buffer_t* command_buffer =
      iree_hal_hip_stream_command_buffer_cast(base_command_buffer);
  IREE_TRACE_ZONE_BEGIN(z0);

  hipDeviceptr_t* current_bindings =
      command_buffer->descriptor_sets[set].bindings;
  for (iree_host_size_t i = 0; i < binding_count; i++) {
    const iree_hal_descriptor_set_binding_t* binding = &bindings[i];
    hipDeviceptr_t device_ptr = NULL;
    if (binding->buffer) {
      IREE_RETURN_AND_END_ZONE_IF_ERROR(
          z0, iree_hal_resource_set_insert(command_buffer->resource_set, 1,
                                           &binding->buffer));

      hipDeviceptr_t device_buffer = iree_hal_hip_buffer_device_pointer(
          iree_hal_buffer_allocated_buffer(binding->buffer));
      iree_device_size_t offset = iree_hal_buffer_byte_offset(binding->buffer);
      device_ptr = (uint8_t*)device_buffer + offset + binding->offset;
    }
    current_bindings[binding->binding] = device_ptr;
  }

  IREE_TRACE_ZONE_END(z0);
  return iree_ok_status();
}

static iree_status_t iree_hal_hip_stream_command_buffer_dispatch(
    iree_hal_command_buffer_t* base_command_buffer,
    iree_hal_executable_t* executable, int32_t entry_point,
    uint32_t workgroup_x, uint32_t workgroup_y, uint32_t workgroup_z) {
  iree_hal_hip_stream_command_buffer_t* command_buffer =
      iree_hal_hip_stream_command_buffer_cast(base_command_buffer);
  IREE_TRACE_ZONE_BEGIN(z0);

  // Lookup kernel parameters used for side-channeling additional launch
  // information from the compiler.
  iree_hal_hip_kernel_info_t kernel_info;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_hal_hip_native_executable_entry_point_kernel_info(
              executable, entry_point, &kernel_info));

  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_hal_resource_set_insert(command_buffer->resource_set, 1,
                                       &executable));

  iree_hal_hip_dispatch_layout_t dispatch_layout =
      iree_hal_hip_pipeline_layout_dispatch_layout(kernel_info.layout);

  // The total number of descriptors across all descriptor sets.
  iree_host_size_t descriptor_count = dispatch_layout.total_binding_count;
  // The total number of push constants.
  iree_host_size_t push_constant_count = dispatch_layout.push_constant_count;
  // We append push constants to the end of descriptors to form a linear chain
  // of kernel arguments.
  iree_host_size_t kernel_params_count = descriptor_count + push_constant_count;
  iree_host_size_t kernel_params_length = kernel_params_count * sizeof(void*);

  // Each kernel_params[i] is itself a pointer to the corresponding
  // element at the *second* inline allocation at the end of the current
  // segment.
  iree_host_size_t total_size = kernel_params_length * 2;
  uint8_t* storage_base = NULL;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_arena_allocate(&command_buffer->arena, total_size,
                              (void**)&storage_base));
  void** params_ptr = (void**)storage_base;

  // Set up kernel arguments to point to the payload slots.
  hipDeviceptr_t* payload_ptr =
      (hipDeviceptr_t*)((uint8_t*)params_ptr + kernel_params_length);
  for (size_t i = 0; i < kernel_params_count; i++) {
    params_ptr[i] = &payload_ptr[i];
  }

  // Copy descriptors from all sets to the end of the current segment for later
  // access.
  iree_host_size_t set_count = dispatch_layout.set_layout_count;
  for (iree_host_size_t i = 0; i < set_count; ++i) {
    // TODO: cache this information in the kernel info to avoid recomputation.
    iree_host_size_t binding_count =
        iree_hal_hip_descriptor_set_layout_binding_count(
            iree_hal_hip_pipeline_layout_descriptor_set_layout(
                kernel_info.layout, i));
    iree_host_size_t index =
        iree_hal_hip_pipeline_layout_base_binding_index(kernel_info.layout, i);
    memcpy(payload_ptr + index, command_buffer->descriptor_sets[i].bindings,
           binding_count * sizeof(hipDeviceptr_t));
  }

  // Append the push constants to the kernel arguments.
  iree_host_size_t base_index = dispatch_layout.push_constant_base_index;
  // As commented in the above, what each kernel parameter points to is a
  // hipDeviceptr_t, which as the size of a pointer on the target machine. we
  // are just storing a 32-bit value for the push constant here instead. So we
  // must process one element each type, for 64-bit machines.
  for (iree_host_size_t i = 0; i < push_constant_count; i++) {
    *((uint32_t*)params_ptr[base_index + i]) =
        command_buffer->push_constants[i];
  }

  dim3 num_blocks = {
      .x = workgroup_x,
      .y = workgroup_y,
      .z = workgroup_z,
  };
  dim3 block_size = {
      .x = kernel_info.block_size[0],
      .y = kernel_info.block_size[1],
      .z = kernel_info.block_size[2],
  };
  IREE_HIP_RETURN_AND_END_ZONE_IF_ERROR(
      z0, command_buffer->hip_symbols,
      hipLaunchKernel(&kernel_info.function, num_blocks, block_size, params_ptr,
                      kernel_info.shared_memory_size,
                      command_buffer->hip_stream),
      "hipLaunchKernel");

  IREE_TRACE_ZONE_END(z0);
  return iree_ok_status();
}

static iree_status_t iree_hal_hip_stream_command_buffer_dispatch_indirect(
    iree_hal_command_buffer_t* base_command_buffer,
    iree_hal_executable_t* executable, int32_t entry_point,
    iree_hal_buffer_t* workgroups_buffer,
    iree_device_size_t workgroups_offset) {
  return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                          "need hip implementation of dispatch indirect");
}

static iree_status_t iree_hal_hip_stream_command_buffer_execute_commands(
    iree_hal_command_buffer_t* base_command_buffer,
    iree_hal_command_buffer_t* base_commands,
    iree_hal_buffer_binding_table_t binding_table) {
  // TODO(#10144): support indirect command buffers with deferred command
  // buffers or graphs. We likely just want to switch to graphs.
  return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                          "indirect command buffers not yet implemented");
}

static const iree_hal_command_buffer_vtable_t
    iree_hal_hip_stream_command_buffer_vtable = {
        .destroy = iree_hal_hip_stream_command_buffer_destroy,
        .begin = iree_hal_hip_stream_command_buffer_begin,
        .end = iree_hal_hip_stream_command_buffer_end,
        .begin_debug_group =
            iree_hal_hip_stream_command_buffer_begin_debug_group,
        .end_debug_group = iree_hal_hip_stream_command_buffer_end_debug_group,
        .execution_barrier =
            iree_hal_hip_stream_command_buffer_execution_barrier,
        .signal_event = iree_hal_hip_stream_command_buffer_signal_event,
        .reset_event = iree_hal_hip_stream_command_buffer_reset_event,
        .wait_events = iree_hal_hip_stream_command_buffer_wait_events,
        .discard_buffer = iree_hal_hip_stream_command_buffer_discard_buffer,
        .fill_buffer = iree_hal_hip_stream_command_buffer_fill_buffer,
        .update_buffer = iree_hal_hip_stream_command_buffer_update_buffer,
        .copy_buffer = iree_hal_hip_stream_command_buffer_copy_buffer,
        .collective = iree_hal_hip_stream_command_buffer_collective,
        .push_constants = iree_hal_hip_stream_command_buffer_push_constants,
        .push_descriptor_set =
            iree_hal_hip_stream_command_buffer_push_descriptor_set,
        .dispatch = iree_hal_hip_stream_command_buffer_dispatch,
        .dispatch_indirect =
            iree_hal_hip_stream_command_buffer_dispatch_indirect,
        .execute_commands = iree_hal_hip_stream_command_buffer_execute_commands,
};