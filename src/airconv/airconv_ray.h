/* what DXR shaders (dxil_converter.cpp), Mullion's runtime for them (dxmt_command.metal) and DispatchRays share.
   plain C, also read as Metal */
#pragma once

#ifdef __METAL__
typedef ulong sm50_ray_qword;
typedef uint sm50_ray_dword;
#else
#include <stdint.h>
typedef uint64_t sm50_ray_qword;
typedef uint32_t sm50_ray_dword;
#endif

/* a ray tracing pipeline's visible function table: the runtime's functions, then shaders, each at the slot the
   device gave it, so that a shader identifier means the same in every state object that has the shader. a shader
   calls the runtime through its slots (TraceRay, ReportHit, CallShader) */
enum SM50_RAY_FUNCTION {
  SM50_RAY_FUNCTION_NONE,
  SM50_RAY_FUNCTION_TRACE,
  SM50_RAY_FUNCTION_REPORT_HIT,
  SM50_RAY_FUNCTION_CALL,
  SM50_RAY_FUNCTION_FIRST_SHADER,
};

/* a shader identifier, which a shader record starts with: its functions' slots, 0 where there is none. a hit group
   has a closest hit, an any hit and an intersection shader, in that order; other shaders are the first. a record's
   local root arguments follow it */
struct SM50_RAY_SHADER_IDENTIFIER {
  sm50_ray_dword function[3];
  sm50_ray_dword reserved[5];
};

/* D3D12_DISPATCH_RAYS_DESC, as the pipeline's kernel gets it */
struct SM50_RAY_DISPATCH {
  sm50_ray_qword ray_generation_record;
  sm50_ray_qword miss_table;
  sm50_ray_qword miss_stride;
  sm50_ray_qword hit_group_table;
  sm50_ray_qword hit_group_stride;
  sm50_ray_qword callable_table;
  sm50_ray_qword callable_stride;
  sm50_ray_dword dimensions[3];
  sm50_ray_dword pipeline_flags; /* ray flags every TraceRay of the pipeline has (D3D12_RAYTRACING_PIPELINE_FLAGS) */
};

/* TraceRay's arguments but the payload, in the order DXIL has them */
struct SM50_RAY_TRACE {
  sm50_ray_qword scene; /* the address of the acceleration structure's memory */
  sm50_ray_dword flags;
  sm50_ray_dword mask;
  sm50_ray_dword ray_contribution;
  sm50_ray_dword geometry_multiplier;
  sm50_ray_dword miss_index;
  float origin[3];
  float tmin;
  float direction[3];
  float tmax;
};

/* an any hit shader's verdict on its hit: it returns having accepted it unless it calls IgnoreHit or
   AcceptHitAndEndSearch */
enum SM50_RAY_VERDICT {
  SM50_RAY_VERDICT_ACCEPT,
  SM50_RAY_VERDICT_IGNORE,
  SM50_RAY_VERDICT_END_SEARCH,
};

/* buffer bindings of the pipeline's kernel, after the root arguments and static samplers every kernel has */
enum SM50_RAY_BINDING {
  SM50_RAY_BINDING_DISPATCH = 2,
  SM50_RAY_BINDING_FUNCTIONS = 3,
};
