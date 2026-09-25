#pragma once

// GLSL sources for the Schwarzschild ray tracer (embedded: no runtime asset paths).
namespace shaders {

inline constexpr const char* kRaytraceVertex = R"glsl(
#version 460 core

out vec2 v_ndc;

void main()
{
  const vec2 k_vertices[3] = vec2[3](
      vec2(-1.0, -1.0),
      vec2( 3.0, -1.0),
      vec2(-1.0,  3.0));
  const vec2 position = k_vertices[gl_VertexID];
  v_ndc = position;
  gl_Position = vec4(position, 0.0, 1.0);
}
)glsl";

inline constexpr const char* kRaytraceFragment = R"glsl(
#version 460 core

in vec2 v_ndc;
out vec4 frag_color;

uniform vec3 u_cam_pos;
uniform vec3 u_cam_fwd;
uniform vec3 u_cam_right;
uniform vec3 u_cam_up;
uniform float u_tan_half_fov;
uniform float u_aspect;
uniform float u_mass;          // M in geometric units (G = c = 1)
uniform float u_proj_near;
uniform float u_proj_far;
uniform float u_disk_inner;    // r_ISCO (6M for Schwarzschild)
uniform float u_disk_outer;
uniform float u_disk_temperature;  // peak emitted temperature [K]
uniform float u_disk_brightness;   // exposure of the emitted flux
uniform float u_time;              // scene clock [s]: churns the disk texture
uniform vec4 u_comet_head[16];     // xyz world position, w = intensity (0 = dead)
uniform vec4 u_comet_trail[128];   // [i*8+k] xyz point, w = fade (0 = none)
uniform vec2 u_resolution;         // framebuffer size [px]
uniform float u_pixel;             // retro block size (1 = native, >1 snaps rays)

const int k_max_steps = 600;
const float k_dphi = 0.02;
const float k_r_escape = 50.0;
const float k_epsilon = 1e-5;
const float k_pi = 3.14159265358979;
const int k_max_hits = 8;
const float k_comet_sigma = 0.14;  // streak halo width [M]
const float k_comet_core = 0.05;   // bright core width [M]
const vec3 k_comet_color = vec3(0.30, 1.0, 0.75);

// ----------------------------------------------------------------------------
// Procedural starfield sampled on the escape direction of each ray.
// ----------------------------------------------------------------------------
float hash13(vec3 p)
{
  p = fract(p * 0.1031);
  p += dot(p, p.zyx + 31.32);
  return fract((p.x + p.y) * p.z);
}

vec3 starfield(vec3 dir)
{
  vec3 color = vec3(0.0);
  for (int layer = 0; layer < 3; ++layer) {
    const float scale = 60.0 * float(layer + 1);
    const vec3 cell = floor(dir * scale);
    const vec3 offset = fract(dir * scale) - 0.5;
    const float h = hash13(cell + float(layer) * 19.7);
    if (h > 0.975) {
      const float brightness =
          smoothstep(0.35, 0.0, length(offset)) * pow((h - 0.975) / 0.025, 2.0);
      const float tint = hash13(cell + 7.13);
      const vec3 star_color = mix(vec3(1.0, 0.8, 0.6), vec3(0.65, 0.8, 1.0), tint);
      color += star_color * brightness * (0.5 + 2.5 * tint);
    }
  }
  return color;
}

// ----------------------------------------------------------------------------
// Blackbody radiation color (Planck locus approximation, Tanner Helland fit).
// ----------------------------------------------------------------------------
vec3 blackbody(float kelvin)
{
  const float t = clamp(kelvin, 1000.0, 15000.0) / 100.0;
  float red;
  float green;
  float blue;
  if (t <= 66.0) {
    red = 255.0;
    green = 99.4708025861 * log(t) - 161.1195681661;
  } else {
    red = 329.698727446 * pow(t - 60.0, -0.1332047592);
    green = 288.1221695283 * pow(t - 60.0, -0.0755148492);
  }
  if (t >= 66.0) {
    blue = 255.0;
  } else if (t <= 19.0) {
    blue = 0.0;
  } else {
    blue = 138.5177312231 * log(t - 10.0) - 305.0447927307;
  }
  return clamp(vec3(red, green, blue) / 255.0, 0.0, 1.0);
}

// ----------------------------------------------------------------------------
// Accretion disk emission (spec sections 4.1-4.2).
//
// g = (u^t_obs) / (u^t_emit * (1 - Omega * lambda)) is the relativistic
// frequency shift; bolometric intensity scales as g^4 (Liouville).
// lambda = L_z / E is the conserved axial angular momentum ratio.
// ----------------------------------------------------------------------------
void disk_emission(float r, float phi, float lambda, out vec3 color,
                   out float alpha)
{
  const float x = u_disk_inner / r;

  // Shakura-Sunyaev-like profile: F ~ (1 - sqrt(r_in/r)) (r_in/r)^3,
  // peak value 0.0566 at r = (49/36) r_in.
  const float flux_shape = (1.0 - sqrt(x)) * x * x * x;
  const float flux_peak = 0.0566;
  const float flux = clamp(flux_shape / flux_peak, 0.0, 1.0);

  // Keplerian angular velocity (a = 0), spec 4.1.
  const float omega = 1.0 / pow(r, 1.5);

  // Fluid 4-velocity normalization, spec 4.1 (g_tphi = 0 for Schwarzschild).
  const float u_emit_t =
      1.0 / sqrt(max(1.0 - 2.0 * u_mass / r - omega * omega * r * r, 1e-6));
  const float r_cam = length(u_cam_pos);
  const float u_obs_t = 1.0 / sqrt(max(1.0 - 2.0 * u_mass / r_cam, 1e-6));

  // Shift factor; clamped as an artistic safety rail against singularities.
  const float g = clamp(u_obs_t / (u_emit_t * (1.0 - omega * lambda)), 0.05, 5.0);

  const float temperature = u_disk_temperature * pow(flux, 0.25);
  // Intrinsic azimuthal turbulence advected by the Keplerian angular
  // velocity: inner rings sweep faster than outer ones, so the pattern
  // shears and the disk visibly spins (sandbox liveliness, texture only -
  // the g^4 physics above is untouched).
  const float advected = phi - omega * u_time * 6.0;
  const float shimmer =
      1.0 + 0.12 * sin(6.0 * advected) +
      0.08 * sin(11.0 * advected - 14.0 * r);

  color = blackbody(g * temperature) * pow(g, 4.0) * flux *
          u_disk_brightness * shimmer;

  const float fade = 1.5;
  alpha = smoothstep(u_disk_inner, u_disk_inner + 0.6, r) *
          (1.0 - smoothstep(u_disk_outer - fade, u_disk_outer, r));
}

// ----------------------------------------------------------------------------
// Null geodesics of the Schwarzschild metric.
//
// In the photon orbital plane the radial equation reduces to (spec section 3):
//     d^2u / dphi^2 + u = 3 M u^2,      u = 1 / r
// integrated with classical RK4.
// ----------------------------------------------------------------------------
vec2 geodesic_deriv(vec2 state)
{
  return vec2(state.y, 3.0 * u_mass * state.x * state.x - state.x);
}

vec3 escape_direction(float u, float du, float phi, vec3 e1, vec3 e2)
{
  const vec3 radial = cos(phi) * e1 + sin(phi) * e2;
  const vec3 tangential = -sin(phi) * e1 + cos(phi) * e2;
  return normalize((-du / (u * u)) * radial + (1.0 / u) * tangential);
}

vec3 trace_ray(vec3 dir, out float surface_distance)
{
  surface_distance = u_proj_far;
  const float r0 = length(u_cam_pos);
  const vec3 e1 = u_cam_pos / r0;
  const float cos_alpha = dot(dir, e1);
  const vec3 tangential = dir - cos_alpha * e1;
  const float sin_alpha = length(tangential);

  // Degenerate purely radial ray: no orbital plane to speak of.
  if (sin_alpha < k_epsilon) {
    return cos_alpha > 0.0 ? starfield(e1) : vec3(0.0);
  }
  const vec3 e2 = tangential / sin_alpha;

  float u = 1.0 / r0;
  // Initial du/dphi from the null first integral (E = 1):
  //   du/dphi = -cos(alpha) / (sin(alpha) * r0 * sqrt(1 - 2M/r0))
  const float lapse = sqrt(max(1.0 - 2.0 * u_mass / r0, 1e-6));
  float du = -cos_alpha / (sin_alpha * r0 * lapse);

  // Conserved axial angular momentum ratio lambda = L_z / E (spec section 3).
  // The traced ray runs camera -> disk, the physical photon runs disk ->
  // camera: L_z flips sign under reversal while E stays positive, hence the
  // crossed order below.
  const float lambda = cross(dir, u_cam_pos).z / lapse;

  // Disk lies in the world z = 0 plane. The photon path z(phi) =
  // (e1.z cos(phi) + e2.z sin(phi)) / u vanishes at phi_base + k*pi.
  const float plane_a = e1.z;
  const float plane_b = e2.z;
  const bool has_crossings = (plane_a * plane_a + plane_b * plane_b) > 1e-8;
  float next_cross = 0.0;
  if (has_crossings) {
    const float phi_base = atan(-plane_a, plane_b);
    next_cross = phi_base + ceil(-phi_base / k_pi) * k_pi;
  }

  float hit_r[k_max_hits];
  float hit_phi[k_max_hits];
  int hit_count = 0;

  float phi = 0.0;
  bool captured = false;

  // Particle glow collected along the bent path: rays passing near a streak
  // glow, so the lensed primary/secondary images come out for free. Frozen
  // once the frontmost disk hit is found so the disk occludes the rest.
  vec3 glow = vec3(0.0);
  bool glow_frozen = false;

  for (int step = 0; step < k_max_steps; ++step) {
    if (u >= 0.5 / u_mass) {
      captured = true;  // crossed the event horizon r_s = 2M
      break;
    }
    if (u <= 1.0 / k_r_escape) {
      break;  // escaped to r > 50 M: sample the starfield
    }

    const float prev_phi = phi;
    const float prev_u = u;

    const vec2 state = vec2(u, du);
    const vec2 k1 = geodesic_deriv(state);
    const vec2 k2 = geodesic_deriv(state + 0.5 * k_dphi * k1);
    const vec2 k3 = geodesic_deriv(state + 0.5 * k_dphi * k2);
    const vec2 k4 = geodesic_deriv(state + k_dphi * k3);
    const vec2 next = state + (k_dphi / 6.0) * (k1 + 2.0 * k2 + 2.0 * k3 + k4);

    phi += k_dphi;
    u = next.x;
    du = next.y;

    // Equatorial crossings inside this step (multiple disk images included).
    while (has_crossings && next_cross <= phi && hit_count < k_max_hits) {
      const float t = clamp((next_cross - prev_phi) / k_dphi, 0.0, 1.0);
      const float u_hit = mix(prev_u, u, t);
      if (u_hit < 0.5 / u_mass) {
        const float r_hit = 1.0 / u_hit;
        if (r_hit >= u_disk_inner && r_hit <= u_disk_outer) {
          hit_r[hit_count] = r_hit;
          hit_phi[hit_count] = next_cross;
          ++hit_count;
        }
      }
      next_cross += k_pi;
    }

    // Sample the particle streaks along the path. Must happen BEFORE the
    // freeze below: a particle on the disk plane crosses z = 0 at its own
    // position, so sampling after the freeze would always miss it.
    if (!glow_frozen) {
      const vec3 p = (1.0 / u) * (cos(phi) * e1 + sin(phi) * e2);
      for (int i = 0; i < 16; ++i) {
        const vec4 head = u_comet_head[i];
        if (head.w <= 0.0) {
          continue;  // dead slot
        }
        const vec3 dh = p - head.xyz;
        const float d2 = dot(dh, dh);
        if (d2 > 100.0) {
          continue;  // farther than 10M: no glow possible
        }
        glow += k_comet_color * head.w *
                (exp(-d2 / (k_comet_sigma * k_comet_sigma)) * 0.7 +
                 exp(-d2 / (k_comet_core * k_comet_core)) * 2.5);
        for (int k = 0; k < 7; ++k) {
          const vec4 a = u_comet_trail[i * 8 + k];
          const vec4 b = u_comet_trail[i * 8 + k + 1];
          if (a.w <= 0.0 || b.w <= 0.0) {
            break;  // trail shorter than this
          }
          const vec3 ab = b.xyz - a.xyz;
          const float t =
              clamp(dot(p - a.xyz, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
          const float dd = length(p - (a.xyz + t * ab));
          const float d = dd * dd;
          glow += k_comet_color * a.w *
                  (exp(-d / (k_comet_sigma * k_comet_sigma)) * 0.7 +
                   exp(-d / (k_comet_core * k_comet_core)) * 2.0);
        }
      }
    }
    // Freeze once the frontmost disk hit is found so the disk occludes the
    // rest of the path (no glow from behind the disk).
    if (hit_count > 0) {
      glow_frozen = true;
    }
  }

  vec3 background = vec3(0.0);
  if (!captured && u <= 0.35 / u_mass) {
    background = starfield(escape_direction(u, du, phi, e1, e2));
  }

  // Frontmost surface along this ray: first disk image, else the horizon.
  if (hit_count > 0) {
    const vec3 p = hit_r[0] * (cos(hit_phi[0]) * e1 + sin(hit_phi[0]) * e2);
    surface_distance = dot(p - u_cam_pos, u_cam_fwd);
  } else if (captured) {
    const vec3 p = (1.0 / u) * (cos(phi) * e1 + sin(phi) * e2);
    surface_distance = dot(p - u_cam_pos, u_cam_fwd);
  }

  // Hits are stored front (camera side, small phi) to back; composite in
  // reverse so the nearest disk image lands on top.
  vec3 color = background;
  for (int i = hit_count - 1; i >= 0; --i) {
    vec3 emission;
    float alpha;
    disk_emission(hit_r[i], hit_phi[i], lambda, emission, alpha);
    color = mix(color, emission, alpha);
  }
  return color + glow;
}

void main()
{
  // Retro mode: every u_pixel x u_pixel block shares one traced ray, so the
  // whole ray-traced layer (shadow, disk, stars, streaks) quantizes at once.
  vec2 ndc = v_ndc;
  if (u_pixel > 1.5) {
    const vec2 px = (ndc * 0.5 + 0.5) * u_resolution;
    ndc = ((floor(px / u_pixel) + 0.5) * u_pixel) / u_resolution * 2.0 - 1.0;
  }
  const vec3 dir = normalize(u_cam_fwd +
                             ndc.x * u_aspect * u_tan_half_fov * u_cam_right +
                             ndc.y * u_tan_half_fov * u_cam_up);
  float surface_distance = u_proj_far;
  vec3 color = trace_ray(dir, surface_distance);

  // Write window depth so later rasterized passes (the spacetime grid)
  // composite correctly against the ray-traced image.
  surface_distance = clamp(surface_distance, u_proj_near, u_proj_far);
  gl_FragDepth =
      (u_proj_far / (u_proj_far - u_proj_near)) *
      (1.0 - u_proj_near / surface_distance);

  // Filmic tone mapping (Narkowicz ACES approximation) + gamma encode.
  const vec3 exposed = color * 1.2;
  color = clamp((exposed * (2.51 * exposed + 0.03)) /
                    (exposed * (2.43 * exposed + 0.59) + 0.14),
                0.0, 1.0);
  frag_color = vec4(pow(color, vec3(1.0 / 2.2)), 1.0);
}
)glsl";

// Spacetime grid: rasterized mesh whose vertices sink into a gravity well.
// Deliberately a stylised vertex warp, not lensed ray tracing (ponytail).
inline constexpr const char* kGridVertex = R"glsl(
#version 460 core

layout(location = 0) in vec3 a_position;

uniform mat4 u_mvp;
uniform float u_mass;
uniform vec2 u_resolution;
uniform float u_pixel;

out float v_radius;

void main()
{
  const float radius = length(a_position.xy);
  vec3 position = a_position;
  // Rubber-sheet depression: z ~ -8M / (r + 2), sitting just below z = 0
  // so the equatorial disk (ray-traced at z = 0) never z-fights with it.
  position.z = -8.0 * u_mass / (radius + 2.0) - 0.02;
  v_radius = radius;
  vec4 clip = u_mvp * vec4(position, 1.0);
  // Same block snap as the ray tracer so the grid joins the retro look.
  if (u_pixel > 1.5) {
    vec2 ndc = clip.xy / clip.w;
    vec2 px = (ndc * 0.5 + 0.5) * u_resolution;
    px = (floor(px / u_pixel) + 0.5) * u_pixel;
    clip.xy = (px / u_resolution * 2.0 - 1.0) * clip.w;
  }
  gl_Position = clip;
}
)glsl";

inline constexpr const char* kGridFragment = R"glsl(
#version 460 core

in float v_radius;
out vec4 frag_color;

uniform float u_grid_max_radius;

void main()
{
  const float fade =
      1.0 - smoothstep(u_grid_max_radius - 10.0, u_grid_max_radius, v_radius);
  frag_color = vec4(vec3(0.2, 0.55, 1.0) * fade, 1.0);
}
)glsl";

// ----------------------------------------------------------------------------
// Polish chain (flag --polish): bright-pass + separable blur at quarter res,
// then a composite that adds the bloom and runs FXAA onto the backbuffer.
// Blur shader shared by both passes; u_texel is the SAMPLING target's texel.
// ----------------------------------------------------------------------------
inline constexpr const char* kBlurFragment = R"glsl(
#version 460 core

in vec2 v_ndc;
out vec4 frag_color;

uniform sampler2D u_scene;
uniform vec2 u_texel;      // texel size of the sampled texture
uniform vec2 u_dir;        // blur direction (1,0) or (0,1)
uniform float u_threshold; // > 0: soft bright-pass (first pass only)

void main()
{
  const vec2 uv = v_ndc * 0.5 + 0.5;
  const vec2 step = u_dir * u_texel;
  vec3 c = texture(u_scene, uv).rgb * 0.227027;
  c += texture(u_scene, uv + step * 1.3846).rgb * 0.316216;
  c += texture(u_scene, uv - step * 1.3846).rgb * 0.316216;
  c += texture(u_scene, uv + step * 3.2308).rgb * 0.070270;
  c += texture(u_scene, uv - step * 3.2308).rgb * 0.070270;
  if (u_threshold > 0.0) {
    const float l = dot(c, vec3(0.299, 0.587, 0.114));
    c *= smoothstep(u_threshold, u_threshold + 0.25, l);
  }
  frag_color = vec4(c, 1.0);
}
)glsl";

inline constexpr const char* kCompositeFragment = R"glsl(
#version 460 core

in vec2 v_ndc;
out vec4 frag_color;

uniform sampler2D u_scene;
uniform sampler2D u_bloom;
uniform float u_bloom_strength;
uniform vec2 u_resolution;
uniform float u_pixel;     // > 1.5: retro blocks, FXAA stays off

vec3 fetch(vec2 uv)
{
  return texture(u_scene, uv).rgb +
         texture(u_bloom, uv).rgb * u_bloom_strength;
}

float luma(vec3 c)
{
  return dot(c, vec3(0.299, 0.587, 0.114));
}

// Compact FXAA: blend across the local edge, keep flat areas untouched.
vec3 fxaa(vec2 uv, vec2 px)
{
  const vec3 rgbM = fetch(uv);
  const vec3 rgbNW = fetch(uv + vec2(-1.0, -1.0) * px);
  const vec3 rgbNE = fetch(uv + vec2( 1.0, -1.0) * px);
  const vec3 rgbSW = fetch(uv + vec2(-1.0,  1.0) * px);
  const vec3 rgbSE = fetch(uv + vec2( 1.0,  1.0) * px);
  const float lM = luma(rgbM);
  const float lNW = luma(rgbNW);
  const float lNE = luma(rgbNE);
  const float lSW = luma(rgbSW);
  const float lSE = luma(rgbSE);
  const float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
  const float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
  if (lMax - lMin < max(0.035, lMax * 0.125)) {
    return rgbM;
  }
  vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), ((lNW + lSW) - (lNE + lSE)));
  const float dir_reduce =
      max((lNW + lNE + lSW + lSE) * 0.03125, 1.0 / 128.0);
  const float rcp_min = 1.0 / (min(abs(dir.x), abs(dir.y)) + dir_reduce);
  dir = clamp(dir * rcp_min, -8.0, 8.0) * px;
  const vec3 rgbA =
      0.5 * (fetch(uv + dir * (1.0 / 3.0 - 0.5)) +
             fetch(uv + dir * (2.0 / 3.0 - 0.5)));
  const vec3 rgbB =
      rgbA * 0.5 + 0.25 * (fetch(uv - dir * 0.5) + fetch(uv + dir * 0.5));
  const float lB = luma(rgbB);
  return (lB < lMin || lB > lMax) ? rgbA : rgbB;
}

void main()
{
  vec2 uv = v_ndc * 0.5 + 0.5;
  if (u_pixel > 1.5) {
    // Retro: sample on the same block centers as the ray tracer so the
    // bloom gradient cannot soften the blocks (FXAA stays off below).
    uv = (floor(uv * u_resolution / u_pixel) + 0.5) * u_pixel / u_resolution;
  }
  const vec3 c = (u_pixel > 1.5) ? fetch(uv) : fxaa(uv, 1.0 / u_resolution);
  frag_color = vec4(c, 1.0);
}
)glsl";

}  // namespace shaders