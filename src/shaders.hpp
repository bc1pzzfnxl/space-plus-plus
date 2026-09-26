#pragma once

// GLSL sources for the Kerr ray tracer (embedded: no runtime asset paths).
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
uniform float u_spin;          // dimensionless spin a* in [-0.95, 0.95]
uniform float u_proj_near;
uniform float u_proj_far;
uniform float u_disk_inner;    // r_ISCO (6M for Schwarzschild)
uniform float u_disk_outer;
uniform float u_disk_temperature;  // peak emitted temperature [K]
uniform float u_disk_brightness;   // exposure of the emitted flux
uniform float u_time;              // scene clock [s]: churns the disk texture
uniform vec4 u_comet_head[16];     // xyz world position, w = intensity (0 = dead)
uniform vec4 u_comet_trail[128];   // [i*8+k] xyz point, w = fade (0 = none)
const int k_star_slots = 3;        // concurrent TDE stars (matches CPU side)
const int k_star_trail = 6;        // history points per star
uniform vec4 u_star_pos_radius[k_star_slots];    // xyz ball, w = radius (0 = off)
uniform vec4 u_star_axis_stretch[k_star_slots];  // xyz spaghett. axis, w = stretch
uniform vec4 u_star_color[k_star_slots];         // rgb tint, w = intensity
uniform vec4 u_star_trail[k_star_slots * k_star_trail];  // [s*6+k] point, w = fade
uniform vec2 u_impact[k_star_slots];             // x = disrupt radius, y = heat
uniform vec2 u_resolution;         // framebuffer size [px]
uniform float u_pixel;             // retro block size (1 = native, >1 snaps rays)
uniform vec4 u_realism;            // x = plunge, y = jets, z = ergo, w = turbulence

const int k_max_steps = 600;  // capped: rays past this are near-trapped photon
                              // orbits; the escape/capture fallback below
                              // resolves them either way
const int k_glow_stride = 4;  // particle glow sampled every Nth step (the halo
                              // is spatially smooth); contributions scaled by N.
                              // Debris-only world: the few live streaks cost
                              // little even at full rate; stride keeps events
                              // from tanking the frame.
const float k_step_r = 0.30;   // max |dr| per Mino-time step
const float k_step_th = 0.10;  // max |dtheta| per Mino-time step
const float k_step_ph = 0.02;  // max |dphi| per Mino-time step
const float k_r_escape = 50.0;
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
// Valid deep into the blue: the power-law branches extrapolate cleanly, so
// true Novikov-Thorne peak temperatures (1e5..1e7 K, physical-colors mode)
// render blue-white without NaNs.
// ----------------------------------------------------------------------------
vec3 blackbody(float kelvin)
{
  const float t = clamp(kelvin, 1000.0, 1.0e8) / 100.0;
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
// Phenomenological disk turbulence: 3-octave value noise advected by the
// Keplerian flow (the pattern corotates, so inner rings shear past outer
// ones). Not GRMHD — a visual stand-in with the right differential shear.
// ----------------------------------------------------------------------------
float turb_hash(vec2 p)
{
  vec3 q = fract(vec3(p.xyx) * 0.1031);
  q += dot(q, q.yzx + 33.33);
  return fract((q.x + q.y) * q.z);
}

float turb_noise(vec2 p)
{
  const vec2 i = floor(p);
  const vec2 f = fract(p);
  const vec2 u = f * f * (3.0 - 2.0 * f);
  return mix(mix(turb_hash(i), turb_hash(i + vec2(1.0, 0.0)), u.x),
             mix(turb_hash(i + vec2(0.0, 1.0)), turb_hash(i + vec2(1.0, 1.0)),
                 u.x),
             u.y);
}

// ----------------------------------------------------------------------------
// Accretion disk emission (spec sections 4.1-4.2).
//
// g = (u^t_obs) / (u^t_emit * (1 - Omega * lambda)) is the relativistic
// frequency shift; bolometric intensity scales as g^4 (Liouville).
// lambda = L_z / E is the conserved axial angular momentum ratio.
// ----------------------------------------------------------------------------
float kerr_a();  // defined with the Kerr tracer below

void disk_emission(float r, float phi, float lambda, float mu,
                   out vec3 color, out float alpha)
{
  const float x = u_disk_inner / r;

  // Shakura-Sunyaev-like profile: F ~ (1 - sqrt(r_in/r)) (r_in/r)^3,
  // peak value 0.0566 at r = (49/36) r_in.
  const float flux_shape = (1.0 - sqrt(x)) * x * x * x;
  const float flux_peak = 0.0566;
  float flux = clamp(flux_shape / flux_peak, 0.0, 1.0);

  // Plunging region (spec section 6): gas between the horizon and the ISCO
  // still radiates, dimmer and tapering to zero at r_plus.
  const float r_plus_e =
      u_mass + sqrt(max(u_mass * u_mass - kerr_a() * kerr_a(), 0.0));
  if (r < u_disk_inner && u_realism.x > 0.5) {
    flux = 0.15 * pow(clamp((r - r_plus_e) / max(u_disk_inner - r_plus_e, 1e-3),
                            0.0, 1.0),
                      1.5);
  }

  // Kerr equatorial circular-orbit angular velocity, spec 4.1:
  // Omega_K = 1 / (r^{3/2} + a); reduces to Keplerian at a = 0.
  const float a = kerr_a();
  const float omega = 1.0 / (pow(r, 1.5) + a);

  // Fluid 4-velocity normalization, spec 4.1, equatorial metric components
  // (sin(theta) = 1): u_e^t = 1 / sqrt(-(g_tt + 2*Omega*g_tphi
  // + Omega^2*g_phiphi)); g_tphi = 0 recovers the Schwarzschild form.
  const float gtt = -(1.0 - 2.0 * u_mass / r);
  const float gtph = -2.0 * u_mass * a / r;
  const float gpp = r * r + a * a + 2.0 * u_mass * a * a / r;
  const float u_emit_t =
      1.0 / sqrt(max(-(gtt + 2.0 * omega * gtph + omega * omega * gpp), 1e-6));
  const float r_cam = length(u_cam_pos);
  const float u_obs_t = 1.0 / sqrt(max(1.0 - 2.0 * u_mass / r_cam, 1e-6));

  // Shift factor; clamped as an artistic safety rail against singularities.
  const float g = clamp(u_obs_t / (u_emit_t * (1.0 - omega * lambda)), 0.05, 5.0);

  const float temperature = u_disk_temperature * pow(flux, 0.25);
  // Corotating turbulent pattern: phase = phi - Omega*t with t in geometric
  // units, so the texture shears at the true Keplerian rate (the g^4 physics
  // above is untouched). Sampled in (cos, sin) space: no azimuth seam.
  const float advected = phi - omega * u_time;
  float shimmer = 1.0;
  if (u_realism.w > 0.5) {
    const vec2 tp = vec2(cos(advected), sin(advected)) * (1.5 + r * 0.35);
    const float n = turb_noise(tp * 1.7) * 0.6 +
                    turb_noise(tp * 3.7 + 13.1) * 0.3 +
                    turb_noise(tp * 7.9 + 71.7) * 0.15;
    shimmer = 0.72 + 0.55 * n;
  }

  color = blackbody(g * temperature) * pow(g, 4.0) * flux *
          u_disk_brightness * shimmer;
  // Eddington limb darkening: face-on (mu = 1) renders full brightness,
  // grazing rays dim toward 0.4x. mu comes from the crossing segment slope.
  color *= (1.0 + 1.5 * clamp(mu, 0.0, 1.0)) / 2.5;
  // TDE heating flashes: localized bumps around each disruption radius,
  // decaying on the CPU side (strength -> 0, skipped entirely when cold).
  for (int s = 0; s < k_star_slots; ++s) {
    if (u_impact[s].y <= 0.01) {
      continue;
    }
    const float du = (r - u_impact[s].x) * 0.5;
    color *= 1.0 + u_impact[s].y * exp(-du * du);
  }

  const float fade = 1.5;
  float alpha_in = smoothstep(u_disk_inner, u_disk_inner + 0.6, r);
  if (r < u_disk_inner && u_realism.x > 0.5) {
    alpha_in = smoothstep(r_plus_e, r_plus_e + 0.3, r);
  }
  alpha = alpha_in * (1.0 - smoothstep(u_disk_outer - fade, u_disk_outer, r));
}

// ----------------------------------------------------------------------------
// Null geodesics of the Kerr metric (spec sections 2.2 and 3.2).
//
// Carter's separated equations in Mino time dtau = dlambda / rho^2:
//   (dr/dtau)^2 = R(r), (dtheta/dtau)^2 = Theta(theta)
// integrated without square roots via the second-order forms
//   d^2r/dtau^2 = R'(r)/2,  d^2theta/dtau^2 = Theta'(theta)/2
// which stay sign-consistent through radial/polar turning points.
// ----------------------------------------------------------------------------
struct KerrState {
  float r;
  float vr;
  float th;
  float vth;
  float ph;
};

float kerr_a()
{
  return u_spin * u_mass;
}

float kerr_delta(float r)
{
  const float a = kerr_a();
  return r * r - 2.0 * u_mass * r + a * a;
}

float kerr_R(float r, float xi, float eta)
{
  const float a = kerr_a();
  const float p = r * r + a * a - a * xi;
  return p * p - kerr_delta(r) * (eta + (xi - a) * (xi - a));
}

float kerr_dR(float r, float xi, float eta)
{
  const float a = kerr_a();
  const float p = r * r + a * a - a * xi;
  const float q = eta + (xi - a) * (xi - a);
  return 4.0 * r * p - (2.0 * r - 2.0 * u_mass) * q;
}

float kerr_T(float th, float xi, float eta)
{
  const float a = kerr_a();
  const float c = cos(th);
  const float s2 = max(sin(th) * sin(th), 1e-8);
  return eta + a * a * c * c - xi * xi * c * c / s2;
}

float kerr_dT(float th, float xi, float eta)
{
  const float a = kerr_a();
  const float c = cos(th);
  const float s = max(sin(th), 1e-3);
  return -2.0 * a * a * c * s + 2.0 * xi * xi * c / (s * s * s);
}

// Mino-time dphi/dtau for E = 1, Lz = xi. Verified against the metric
// contraction rho^2 * g^{mu phi} p_mu (and dphi/dtau = rho^2 * U^phi):
// xi/sin^2 - a + a(r^2+a^2-a xi)/Delta. Spec section 3.2's quoted
// expression differs by +a.
float kerr_dphi(float r, float th, float xi)
{
  const float a = kerr_a();
  const float s = max(sin(th), 1e-3);
  return (xi / (s * s) - a) +
         a * (r * r + a * a - a * xi) / max(kerr_delta(r), 1e-6);
}

KerrState kerr_deriv(const KerrState s, float xi, float eta)
{
  KerrState d;
  d.r = s.vr;
  d.vr = 0.5 * kerr_dR(s.r, xi, eta);
  d.th = s.vth;
  d.vth = 0.5 * kerr_dT(s.th, xi, eta);
  d.ph = kerr_dphi(s.r, s.th, xi);
  return d;
}

KerrState kerr_scale(const KerrState s, float k)
{
  KerrState o;
  o.r = s.r * k;
  o.vr = s.vr * k;
  o.th = s.th * k;
  o.vth = s.vth * k;
  o.ph = s.ph * k;
  return o;
}

KerrState kerr_add(const KerrState p, const KerrState q)
{
  KerrState o;
  o.r = p.r + q.r;
  o.vr = p.vr + q.vr;
  o.th = p.th + q.th;
  o.vth = p.vth + q.vth;
  o.ph = p.ph + q.ph;
  return o;
}

vec3 bl_to_cart(float r, float th, float ph)
{
  const float a = kerr_a();
  const float s = sqrt(r * r + a * a);
  return vec3(s * sin(th) * cos(ph), s * sin(th) * sin(ph), r * cos(th));
}

vec3 trace_ray(vec3 dir, out float surface_distance)
{
  surface_distance = u_proj_far;
  const float M = u_mass;
  const float a = kerr_a();

  // Camera position in Boyer-Lindquist coordinates:
  // z = r cos(theta), x^2 + y^2 = (r^2 + a^2) sin^2(theta).
  const float rad2 = dot(u_cam_pos, u_cam_pos);
  const float q = rad2 - a * a;
  const float r0 =
      sqrt(max(0.5 * (q + sqrt(q * q + 4.0 * a * a * u_cam_pos.z * u_cam_pos.z)),
               1e-8));
  const float ct = clamp(u_cam_pos.z / r0, -1.0, 1.0);
  const float th0 = acos(ct);
  const float ph0 = atan(u_cam_pos.y, u_cam_pos.x);
  const float st = max(sin(th0), 1e-4);
  const float cf = cos(ph0);
  const float sf = sin(ph0);

  // Chain rule BL -> Cartesian for the unit view direction.
  const float big_s = sqrt(r0 * r0 + a * a);
  const float sig = r0 * r0 + a * a * ct * ct;
  const float cpar = dir.x * cf + dir.y * sf;
  const float drt = big_s * (r0 * st * cpar + big_s * ct * dir.z) / sig;
  const float dtht = (big_s * cpar * ct - r0 * st * dir.z) / sig;
  const float dph = (-dir.x * sf + dir.y * cf) / (big_s * st);

  // Metric coefficients at the camera + static-observer coframe components
  // of the view direction (spec section 2.2).
  const float del = max(kerr_delta(r0), 1e-8);
  const float f = max(1.0 - 2.0 * M * r0 / sig, 1e-6);  // -g_tt
  const float sin2 = st * st;
  const float g_tphi = -2.0 * M * r0 * a * sin2 / sig;
  const float gphiphi =
      sin2 * (r0 * r0 + a * a + 2.0 * M * r0 * a * a * sin2 / sig);
  const float h = gphiphi + g_tphi * g_tphi / f;

  vec3 n = vec3(sqrt(sig / del) * drt, sqrt(sig) * dtht, sqrt(h) * dph);
  const float nlen = length(n);
  n = nlen > 1e-8 ? n / nlen : vec3(1.0, 0.0, 0.0);

  // Conserved quantities of the traced branch (E = 1, future-directed along
  // the view direction): Lz = g_tphi/f + n.z*sqrt(h)/sqrt(f). At a = 0 this
  // is +cross(cam, dir).z / lapse, so phi advances along the traced path.
  const float xi = g_tphi / f + n.z * sqrt(h) / sqrt(f);
  const float eta =
      sig * n.y * n.y / f - a * a * ct * ct + xi * xi * ct * ct / (st * st);

  KerrState s;
  s.r = r0;
  s.th = th0;
  s.ph = ph0;
  s.vr = (n.x >= 0.0 ? 1.0 : -1.0) * sqrt(max(kerr_R(r0, xi, eta), 0.0));
  s.vth = (n.y >= 0.0 ? 1.0 : -1.0) * sqrt(max(kerr_T(th0, xi, eta), 0.0));

  const float r_plus = M + sqrt(max(M * M - a * a, 0.0));

  float hit_r[k_max_hits];
  float hit_ph[k_max_hits];
  float hit_mu[k_max_hits];  // incidence cosine at the crossing (limb dark.)
  int hit_count = 0;
  bool captured = false;
  bool escaped = false;

  // Particle glow collected along the bent path: rays passing near a streak
  // glow, so the lensed primary/secondary images come out for free. Frozen
  // once the frontmost disk hit is found so the disk occludes the rest.
  vec3 glow = vec3(0.0);
  bool glow_frozen = false;
  vec3 jet = vec3(0.0);
  float ergo = 0.0;

  for (int step = 0; step < k_max_steps; ++step) {
    if (s.r <= r_plus + 1e-4 * M) {
      captured = true;
      break;
    }
    // Escape only when outside AND moving outward: the camera itself may sit
    // beyond k_r_escape (zoom-out), and rays aimed at the hole must still
    // integrate inward instead of dying on step 0.
    if (s.r >= k_r_escape && s.vr > 0.0) {
      escaped = true;
      break;
    }

    // Adaptive Mino step: cap motion in r/theta/phi AND the velocity change.
    // R' and Theta' spike near the turning points; without the velocity caps
    // the RK4 midpoint overshoots the turn, vr/vth explode, the ray fakes
    // hundreds of pole reflections and floods fake disk hits (rib column).
    // Far field: the radial cap scales with r (2 % per step) so rays from a
    // distant camera cross the weak field without exhausting the budget;
    // near the hole the 0.3 M cap still rules (accuracy where it matters).
    const float rv = max(kerr_R(s.r, xi, eta), 0.0);
    const float tv = max(kerr_T(s.th, xi, eta), 0.0);
    const float fr = kerr_dphi(s.r, s.th, xi);
    const float step_r = max(k_step_r, 0.02 * s.r);
    float dtau = min(step_r / max(sqrt(rv), 1e-4),
                     k_step_th / max(sqrt(tv), 1e-4));
    dtau = min(dtau,
               0.2 * max(abs(s.vr), 0.2) /
                   max(abs(kerr_dR(s.r, xi, eta)), 1.0));
    dtau = min(dtau,
               0.2 * max(abs(s.vth), 0.2) /
                   max(abs(kerr_dT(s.th, xi, eta)), 1.0));
    dtau = min(dtau, k_step_ph / max(abs(fr), 1e-4));
    dtau = clamp(dtau, 1e-5, 0.05);

    const KerrState prev = s;
    const KerrState k1 = kerr_deriv(s, xi, eta);
    const KerrState k2 =
        kerr_deriv(kerr_add(s, kerr_scale(k1, 0.5 * dtau)), xi, eta);
    const KerrState k3 =
        kerr_deriv(kerr_add(s, kerr_scale(k2, 0.5 * dtau)), xi, eta);
    const KerrState k4 = kerr_deriv(kerr_add(s, kerr_scale(k3, dtau)), xi, eta);
    s = kerr_add(
        s, kerr_scale(kerr_add(k1, kerr_add(kerr_scale(k2, 2.0),
                                            kerr_add(kerr_scale(k3, 2.0), k4))),
                      dtau / 6.0));
    // Polar crossing: in Boyer-Lindquist a path through the axis maps
    // theta -> -theta (or 2pi - theta) with phi -> phi + pi.
    if (s.th < 0.0) {
      s.th = -s.th;
      s.vth = -s.vth;
      s.ph += k_pi;
    } else if (s.th > k_pi) {
      s.th = 2.0 * k_pi - s.th;
      s.vth = -s.vth;
      s.ph += k_pi;
    }

    // Ergosphere surface r_E(theta) = M + sqrt(M^2 - a^2 cos^2 theta)
    // (spec 2.3): glow where the ray pierces it, outside-in only.
    if (u_realism.z > 0.5) {
      const float ce = cos(s.th);
      const float ce_p = cos(prev.th);
      const float r_e = M + sqrt(max(M * M - a * a * ce * ce, 0.0));
      const float r_e_p = M + sqrt(max(M * M - a * a * ce_p * ce_p, 0.0));
      if (r_e > r_plus + 1e-3 * M && prev.r > r_e_p && s.r <= r_e) {
        ergo += 1.0;
      }
    }

    // Polar jets: soft cone along the rotation axis with advected knots.
    // Decorative clock (jets are not physical here): kept at the historical
    // visual speed, rescaled for the geometric time unit (4M per frame).
    if (u_realism.y > 0.5) {
      const float zj = abs(s.r * cos(s.th));
      const float rho = s.r * sin(s.th);
      const float wid = 0.3 + 0.09 * zj;
      if (rho < wid && zj < 45.0) {
        const float core = pow(1.0 - rho / wid, 2.0);
        const float axial = smoothstep(1.5, 4.0, zj) *
                            (1.0 - smoothstep(30.0, 45.0, zj));
        const float knots = 0.7 + 0.3 * sin(2.5 * zj - u_time * 0.025);
        jet += vec3(0.45, 0.65, 1.0) * core * axial * knots *
               min(dtau, 0.05) * 2.5;
      }
    }

    // Disk slab crossings: the disk has a flared half-thickness H(r) = 0.1 r
    // (puffed silhouette instead of a razor plane). One hit per traversal,
    // recorded at the slab ENTRY and merged with a same-step midplane
    // crossing; a step jumping the whole slab falls back to the midplane
    // point. A full traversal later also logs its midplane hit, so the
    // sightline integral is sampled twice — brighter, as a slab should be.
    // mu is the true incidence cosine from the Cartesian crossing segment.
    const float zp = prev.r * cos(prev.th);
    const float zc = s.r * cos(s.th);
    const float fp = abs(zp) - 0.1 * prev.r;
    const float fc = abs(zc) - 0.1 * s.r;
    const bool entered = (fp > 0.0 && fc <= 0.0);
    const bool midplane = (cos(prev.th) * cos(s.th) < 0.0);
    if ((entered || midplane) && hit_count < k_max_hits) {
      float t = 0.0;
      if (entered && !midplane) {
        t = clamp(fp / (fp - fc), 0.0, 1.0);
      } else {
        t = clamp((0.5 * k_pi - prev.th) / (s.th - prev.th), 0.0, 1.0);
      }
      const float rh = mix(prev.r, s.r, t);
      const float r_floor = u_realism.x > 0.5 ? r_plus + 1e-3 * M : u_disk_inner;
      if (rh > r_plus && rh >= r_floor && rh <= u_disk_outer) {
        hit_r[hit_count] = rh;
        hit_ph[hit_count] = mix(prev.ph, s.ph, t);
        const vec3 pc = bl_to_cart(prev.r, prev.th, prev.ph);
        const vec3 qc = bl_to_cart(s.r, s.th, s.ph);
        hit_mu[hit_count] =
            clamp(abs(qc.z - pc.z) / max(length(qc - pc), 1e-6), 0.0, 1.0);
        ++hit_count;
      }
    }

    // Sample the particle streaks along the path. Must happen BEFORE the
    // freeze below: a particle on the disk plane crosses z = 0 at its own
    // position, so sampling after the freeze would always miss it.
    // Perf: strided sampling (glow_gain compensates) + distance-gated
    // falloff — beyond 0.5M both terms are < 3e-6 (invisible after the
    // tonemapper), so the exp() pair is skipped entirely; the bright core
    // only matters inside 0.18M.
    if (!glow_frozen && (step % k_glow_stride) == 0) {
      const float glow_gain = float(k_glow_stride);
      const vec3 p = bl_to_cart(s.r, s.th, s.ph);
      for (int i = 0; i < 16; ++i) {
        const vec4 head = u_comet_head[i];
        if (head.w <= 0.0) {
          continue;  // dead slot
        }
        const vec3 dh = p - head.xyz;
        const float d2 = dot(dh, dh);
        if (d2 < 0.25) {
          glow += k_comet_color * head.w * glow_gain *
                  exp(-d2 / (k_comet_sigma * k_comet_sigma)) * 0.7;
          if (d2 < 0.0324) {
            glow += k_comet_color * head.w * glow_gain *
                    exp(-d2 / (k_comet_core * k_comet_core)) * 2.5;
          }
        }
        // Trail: newest 4 segments only (the bright head of the streak).
        for (int k = 0; k < 4; ++k) {
          const vec4 a2 = u_comet_trail[i * 8 + k];
          const vec4 b2 = u_comet_trail[i * 8 + k + 1];
          if (a2.w <= 0.0 || b2.w <= 0.0) {
            break;  // trail shorter than this
          }
          const vec3 ab = b2.xyz - a2.xyz;
          const float tt =
              clamp(dot(p - a2.xyz, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
          const vec3 rel = p - (a2.xyz + tt * ab);
          const float d = dot(rel, rel);
          if (d < 0.25) {
            glow += k_comet_color * a2.w * glow_gain *
                    exp(-d / (k_comet_sigma * k_comet_sigma)) * 0.7;
            if (d < 0.0324) {
              glow += k_comet_color * a2.w * glow_gain *
                      exp(-d / (k_comet_core * k_comet_core)) * 2.0;
            }
          }
        }
      }
      // TDE stars: soft balls, spaghettified along their velocity axes.
      // Distance-gated like the comets: one exp only when near.
      for (int s = 0; s < k_star_slots; ++s) {
        if (u_star_pos_radius[s].w <= 0.0) {
          continue;  // dormant slot
        }
        const vec3 dp = p - u_star_pos_radius[s].xyz;
        const float along = dot(dp, u_star_axis_stretch[s].xyz);
        const vec3 perp = dp - along * u_star_axis_stretch[s].xyz;
        const float st = max(u_star_axis_stretch[s].w, 1.0);
        const float ds = dot(perp, perp) + along * along / (st * st);
        const float sr = u_star_pos_radius[s].w;
        if (ds < 9.0 * sr * sr) {
          glow += u_star_color[s].rgb * u_star_color[s].a * glow_gain *
                  exp(-ds / (sr * sr * 0.5));
        }
        // Luminous trail: history points with fading intensity (w = 0 ends
        // the streak). Narrower than the ball, comet-streak look.
        for (int k = 0; k < k_star_trail; ++k) {
          const vec4 tp = u_star_trail[s * k_star_trail + k];
          if (tp.w <= 0.0) {
            break;
          }
          const vec3 dq = p - tp.xyz;
          const float dq2 = dot(dq, dq);
          if (dq2 < 4.0) {
            glow += u_star_color[s].rgb * tp.w * glow_gain *
                    exp(-dq2 / 0.36);
          }
        }
      }
    }
    // Freeze once the frontmost disk hit is found so the disk occludes the
    // rest of the path (no glow from behind the disk).
    if (hit_count > 0) {
      glow_frozen = true;
    }
  }

  // Step exhaustion fallback: resolve the ray one way or the other so it
  // never renders as an undefined black dot.
  if (!escaped && !captured) {
    if (s.r > 10.0) {
      escaped = true;
    } else {
      captured = true;
    }
  }

  vec3 background = vec3(0.0);
  if (escaped) {
    // Velocity in Boyer-Lindquist pushed forward to Cartesian coordinates.
    const float esc_s = sqrt(s.r * s.r + a * a);
    const float esc_st = max(sin(s.th), 1e-4);
    const float esc_ct = cos(s.th);
    const float esc_dph = kerr_dphi(s.r, s.th, xi);
    const float dx = (s.r / esc_s) * esc_st * cos(s.ph) * s.vr +
                     esc_s * esc_ct * cos(s.ph) * s.vth -
                     esc_s * esc_st * sin(s.ph) * esc_dph;
    const float dy = (s.r / esc_s) * esc_st * sin(s.ph) * s.vr +
                     esc_s * esc_ct * sin(s.ph) * s.vth +
                     esc_s * esc_st * cos(s.ph) * esc_dph;
    const float dz = esc_ct * s.vr - s.r * esc_st * s.vth;
    const float el = length(vec3(dx, dy, dz));
    background = starfield(el > 1e-8 ? vec3(dx, dy, dz) / el : u_cam_fwd);
  }

  // Frontmost surface along this ray: first disk image, else the horizon.
  if (hit_count > 0) {
    const vec3 p = bl_to_cart(hit_r[0], 0.5 * k_pi, hit_ph[0]);
    surface_distance = dot(p - u_cam_pos, u_cam_fwd);
  } else if (captured) {
    const vec3 p = bl_to_cart(s.r, s.th, s.ph);
    surface_distance = dot(p - u_cam_pos, u_cam_fwd);
  }

  // Hits are stored front (camera side, small phi) to back; composite in
  // reverse so the nearest disk image lands on top.
  vec3 color = background;
  for (int i = hit_count - 1; i >= 0; --i) {
    vec3 emission;
    float alpha;
    // lambda for the g-factor is the arriving photon's Lz/E = -xi
    // (the traced branch runs the same curve in the opposite direction).
    disk_emission(hit_r[i], hit_ph[i], -xi, hit_mu[i], emission, alpha);
    color = mix(color, emission, alpha);
  }
  // Ergosphere glow only on rays that pierce and escape: captured rays all
  // cross r_E once on the way in, which would paint the whole shadow green.
  if (escaped) {
    color += ergo * vec3(0.15, 0.9, 0.45) * 0.3;
  }
  return color + glow + jet;
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
uniform float u_grid_max_radius;
uniform vec2 u_resolution;
uniform float u_pixel;

out float v_radius;

void main()
{
  const float radius = length(a_position.xy);
  vec3 position = a_position;
  // Flamm paraboloid: the exact equatorial Schwarzschild embedding
  // w(r) = 2*sqrt(rs*(r - rs)), hung from the rim so the funnel deepens
  // toward the horizon. No Euclidean embedding exists inside rs: the pit
  // continues linearly to a floor (capped, noted). Still a raster overlay,
  // not lensed ray tracing — shape only. The -0.02 keeps it just below the
  // ray-traced disk (z = 0) so they never z-fight.
  const float rs = 2.0 * u_mass;
  const float w_rim = 2.0 * sqrt(rs * max(u_grid_max_radius - rs, 0.0));
  float w = 2.0 * sqrt(rs * max(radius - rs, 0.0));
  if (radius < rs) {
    w = -6.0 * (rs - radius);
  }
  position.z = -(w_rim - w) - 0.02;
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