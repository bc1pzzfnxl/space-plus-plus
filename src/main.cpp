#include <GL/glew.h>
#include <SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>

#include <cmath>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "shaders.hpp"

namespace {

constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;
constexpr int kGlMajorVersion = 4;
constexpr int kGlMinorVersion = 6;

struct SdlWindowDeleter {
  void operator()(SDL_Window* window) const noexcept
  {
    if (window) {
      SDL_DestroyWindow(window);
    }
  }
};

using WindowPtr = std::unique_ptr<SDL_Window, SdlWindowDeleter>;

// RAII wrapper around an SDL OpenGL context (R.1).
class GlContext {
public:
  explicit GlContext(SDL_Window& window)
      : context_{SDL_GL_CreateContext(&window)}
  {
    if (!context_) {
      throw std::runtime_error{"SDL_GL_CreateContext failed: " + std::string{SDL_GetError()}};
    }
  }

  ~GlContext() { SDL_GL_DeleteContext(context_); }

  GlContext(const GlContext&) = delete;
  GlContext& operator=(const GlContext&) = delete;
  GlContext(GlContext&& other) noexcept
      : context_{std::exchange(other.context_, nullptr)}
  {
  }
  GlContext& operator=(GlContext&& other) noexcept
  {
    if (this != &other) {
      SDL_GL_DeleteContext(context_);
      context_ = std::exchange(other.context_, nullptr);
    }
    return *this;
  }

  [[nodiscard]] SDL_GLContext handle() const noexcept { return context_; }

private:
  SDL_GLContext context_;
};

// Mass in geometric units (G = c = 1): the scene is scaled so that M = 1,
// hence r_s = 2, r_ISCO = 6, r_photon = 3 (spec sections 1-2).
constexpr float k_geometric_mass = 1.0F;
constexpr float k_projection_near = 0.1F;
constexpr float k_projection_far = 200.0F;
constexpr float k_grid_max_radius = 45.0F;
constexpr int k_grid_line_count = 61;    // lines per axis
constexpr int k_grid_segment_count = 180; // segments per line (deep funnel curvature)
constexpr float k_mouse_sensitivity = 0.005F;  // radians per pixel
constexpr float k_zoom_step = 0.9F;
constexpr float k_min_orbit_distance = 8.0F;
constexpr float k_max_orbit_distance = 150.0F;
constexpr float k_max_elevation = 1.5F;  // rad, shy of the poles

struct Camera {
  glm::vec3 position{0.0F, -30.0F, 8.0F};
  glm::vec3 target{0.0F, 0.0F, 0.0F};
  float vertical_fov_degrees = 60.0F;

  [[nodiscard]] glm::vec3 forward() const noexcept
  {
    return glm::normalize(target - position);
  }
  [[nodiscard]] glm::vec3 right() const noexcept
  {
    return glm::normalize(glm::cross(forward(), k_world_up));
  }
  [[nodiscard]] glm::vec3 up() const noexcept
  {
    return glm::normalize(glm::cross(right(), forward()));
  }

private:
  static constexpr glm::vec3 k_world_up{0.0F, 0.0F, 1.0F};
};

// Accretion disk parameters (spec section 4): the inner edge IS the Kerr
// ISCO (derived, no override); the outer edge is a free modeling choice
// (the spec fixes no r_out); spin and Eddington ratio are free parameters.
struct DiskParams {
  float inner_radius = 6.0F;
  float outer_radius = 16.0F;
  float peak_temperature = 5200.0F;
  float brightness = 1.2F;      // exposure (artistic, no formula behind it)
  float eddington_ratio = 1.0F;  // mdot = Mdot/Mdot_Edd in [0.01, 1]
};

// Kerr ISCO (spec section 2.2); sign(a) selects the co-rotating family
// (r = 6M at a = 0, ~2.32M at a* = 0.9, ~8.72M retrograde at a* = -0.9).
[[nodiscard]] float kerr_isco(float a)
{
  a = std::clamp(a, -0.998F, 0.998F);
  const float z1 = 1.0F + std::cbrt(1.0F - a * a) *
                               (std::cbrt(1.0F + a) + std::cbrt(1.0F - a));
  const float z2 = std::sqrt(3.0F * a * a + z1 * z1);
  const float root =
      std::sqrt(std::max((3.0F - z1) * (3.0F + z1 + 2.0F * z2), 0.0F));
  return 3.0F + z2 - (a >= 0.0F ? root : -root);
}

// Mouse-driven spherical orbit, mapped onto a Camera every frame.
struct OrbitState {
  glm::vec3 target{0.0F, 0.0F, 0.0F};
  float azimuth = -1.5707963F;  // start on the -y side, matching M2 framing
  float elevation = 0.26F;
  float distance = 31.0F;
  float vertical_fov_degrees = 60.0F;
  bool auto_orbit = true;        // slow constant azimuth drift
  float orbit_speed = 0.08F;     // rad/s (0 stops, negative reverses)

  void orbit(float delta_azimuth, float delta_elevation) noexcept
  {
    azimuth += delta_azimuth;
    elevation = std::clamp(elevation + delta_elevation, -k_max_elevation,
                           k_max_elevation);
  }

  // Advances the auto-orbit by one 60 Hz frame; wrapped so the azimuth
  // slider (clamped to +-pi) never fights the drift.
  void tick_auto_orbit() noexcept
  {
    constexpr float k_pi = 3.14159265358979F;
    azimuth += orbit_speed / 60.0F;
    while (azimuth > k_pi) {
      azimuth -= 2.0F * k_pi;
    }
    while (azimuth < -k_pi) {
      azimuth += 2.0F * k_pi;
    }
  }

  void zoom(int wheel_steps) noexcept
  {
    distance = std::clamp(distance * std::pow(k_zoom_step, static_cast<float>(wheel_steps)),
                          k_min_orbit_distance, k_max_orbit_distance);
  }

  [[nodiscard]] Camera to_camera() const noexcept
  {
    const float ring = distance * std::cos(elevation);
    Camera camera;
    camera.target = target;
    camera.vertical_fov_degrees = vertical_fov_degrees;
    camera.position = target + glm::vec3{ring * std::cos(azimuth),
                                         ring * std::sin(azimuth),
                                         distance * std::sin(elevation)};
    return camera;
  }
};

// Timelike particles (spec section 3): massive particles follow the
// Schwarzschild effective potential
//     (du/dphi)^2 = e^2 - (1 - 2u)(u^2 + l),   u = 1 / r
// whose RK4 form is u' = w, w' = -u + 3u^2 + l (M = 1). Orbits near the
// circular value at their spawn radius precess and never die; low-l
// particles dive through the barrier and are swallowed. Captured or
// escaped slots are re-seeded, so the swarm lives forever.
struct Comet {
  float u = 0.0F;
  float w = 0.0F;
  float phi = 0.0F;
  float l = 0.0F;  // squared specific angular momentum (const of motion)
  glm::vec3 e1{1.0F, 0.0F, 0.0F};  // orbital plane basis (world)
  glm::vec3 e2{0.0F, 1.0F, 0.0F};
  glm::vec3 head{};
  std::array<glm::vec3, 8> trail{};  // [0] = newest
  int trail_len = 0;
  bool alive = false;
};

// Timelike debris slots for tidal disruption events: one fixed dphi step
// per rendered frame; dead slots stay dead (no ambient swarm, no top-up),
// so the glow shader skips everything at rest.
struct CometSystem {
  static constexpr int k_count = 16;
  static constexpr int k_trail = 8;
  static constexpr float k_dphi_frame = 0.012F;  // rad per animation frame
  static constexpr int k_substeps = 2;           // RK4 substeps per frame
  static constexpr float k_escape_u = 1.0F / 45.0F;  // sandbox edge r = 45M

  std::array<Comet, k_count> comets{};
  std::array<glm::vec4, k_count> head_uniforms{};                 // xyz + intensity
  std::array<glm::vec4, k_count * k_trail> trail_uniforms{};      // xyz + fade

  bool running = true;         // event clock (Pause button, Drop auto-starts)
  bool visible = true;         // purist mode hides the glow, sim keeps running
  float speed = 1.0F;          // simulated steps per rendered frame
  float time_scale = 1.0F;     // Kepler clock (1/ sqrt(M/M_sun)): heavier = slower
  float eccentricity = 0.3F;   // debris orbit variety: distance from circular
  float max_inclination = 2.4F;  // debris orbit variety
  int captured = 0;
  int frames = 0;

  // Debris backend for tidal disruption events: the field starts empty and
  // only ever holds TDE debris (seed_debris). No ambient swarm, no top-up:
  // dead slots stay dead, so the glow shader skips everything at rest.
  CometSystem() = default;

  // Places an (already seeded) comet at point: its orbital plane is rotated
  // to contain the position vector, tilted up to +-tilt_max around the
  // radial axis. Used by TDE debris for the explosion cone.
  void orient_at(Comet& comet, const glm::vec3& point, float tilt_max) noexcept
  {
    const float radius = glm::length(point);
    const glm::vec3 radial = point / radius;
    glm::vec3 seed_axis{0.0F, 0.0F, 1.0F};
    if (std::abs(glm::dot(radial, seed_axis)) > 0.9F) {
      seed_axis = {1.0F, 0.0F, 0.0F};
    }
    const glm::vec3 n0 = glm::normalize(glm::cross(radial, seed_axis));
    const glm::vec3 n1 = glm::cross(n0, radial);
    const float tilt = tilt_max * (2.0F * unit_(rng_) - 1.0F);
    const glm::vec3 normal =
        std::cos(tilt) * n0 + std::sin(tilt) * n1;  // stays perpendicular
    comet.e1 = glm::normalize(glm::cross(normal, glm::vec3{0.0F, 0.0F, 1.0F}));
    comet.e2 = glm::cross(normal, comet.e1);
    comet.phi = std::atan2(glm::dot(point, comet.e2), glm::dot(point, comet.e1));
    comet.head = point;
    comet.trail[0] = point;
    comet.trail_len = 1;
    comet.alive = true;
  }

  // TDE debris: count fresh orbits born AT point inside an explosion cone
  // (not at a random radius like ambient spawns). Dead slots first, else
  // round-robin overwrite like spawn().
  void seed_debris(const glm::vec3& point, int count, float cone) noexcept
  {
    const float r = glm::length(point);
    for (int n = 0; n < count; ++n) {
      const int slot = take_slot();
      Comet& comet = comets[static_cast<std::size_t>(slot)];
      seed_orbit_geometry(comet, 1.0F / r);
      orient_at(comet, point, cone);
      publish(slot);
    }
  }

  // Advances the simulation by `speed` steps per rendered frame, carrying the
  // fractional remainder so 0.25x/0.5x slow motion stays smooth.
  void advance() noexcept
  {
    accumulator_ += speed * time_scale;
    int steps = 0;
    while (accumulator_ >= 1.0F && steps < 8) {
      step();
      accumulator_ -= 1.0F;
      ++steps;
    }
    if (steps == 8) {
      accumulator_ = 0.0F;  // runaway guard (speed slider left at max)
    }
  }

  // Advances every live particle by one animation frame (frame-by-frame).
  // Debris only: no top-up, no reseeding. Captured or escaped debris dies
  // for good (alive_count falls back to zero on its own).
  void step() noexcept
  {
    ++frames;
    const float h = k_dphi_frame / static_cast<float>(k_substeps);
    for (int i = 0; i < k_count; ++i) {
      const auto index = static_cast<std::size_t>(i);
      Comet& comet = comets[index];
      if (!comet.alive) {
        continue;
      }
      for (int s = 0; s < k_substeps; ++s) {
        // RK4 on (u, w): u' = w, w' = -u + 3u^2 + l (M = 1).
        const auto deriv_w = [](float u, float l) noexcept {
          return -u + 3.0F * u * u + l;
        };
        const float k1u = comet.w;
        const float k1w = deriv_w(comet.u, comet.l);
        const float u2 = comet.u + 0.5F * h * k1u;
        const float w2 = comet.w + 0.5F * h * k1w;
        const float k2u = w2;
        const float k2w = deriv_w(u2, comet.l);
        const float u3 = comet.u + 0.5F * h * k2u;
        const float w3 = comet.w + 0.5F * h * k2w;
        const float k3u = w3;
        const float k3w = deriv_w(u3, comet.l);
        const float u4 = comet.u + h * k3u;
        const float w4 = comet.w + h * k3w;
        const float k4u = w4;
        const float k4w = deriv_w(u4, comet.l);
        comet.u += (h / 6.0F) * (k1u + 2.0F * k2u + 2.0F * k3u + k4u);
        comet.w += (h / 6.0F) * (k1w + 2.0F * k2w + 2.0F * k3w + k4w);
      }
      comet.phi += k_dphi_frame;

      // End of life: swallowed by the horizon, or slingshot past the
      // sandbox edge. Debris dies for good (no ambient reseeding).
      if (comet.u >= 0.5F || comet.u <= k_escape_u || comet.u <= 0.0F) {
        if (comet.u >= 0.5F) {
          ++captured;
        }
        comet.alive = false;
        publish(i);
        continue;
      }
      // Push the head onto the trail history (newest first).
      for (int k = k_trail - 1; k > 0; --k) {
        comet.trail[static_cast<std::size_t>(k)] =
            comet.trail[static_cast<std::size_t>(k - 1)];
      }
      comet.head = (1.0F / comet.u) *
                   (std::cos(comet.phi) * comet.e1 +
                    std::sin(comet.phi) * comet.e2);
      comet.trail[0] = comet.head;
      comet.trail_len = std::min(comet.trail_len + 1, k_trail);
      publish(i);
    }
  }

  [[nodiscard]] int alive_count() const noexcept
  {
    int alive = 0;
    for (const Comet& comet : comets) {
      if (comet.alive) {
        ++alive;
      }
    }
    return alive;
  }

private:
  // First dead slot, else round-robin overwrite (field full).
  [[nodiscard]] int take_slot() noexcept
  {
    for (int i = 0; i < k_count; ++i) {
      if (!comets[static_cast<std::size_t>(i)].alive) {
        return i;
      }
    }
    const int slot = round_robin_ % k_count;
    ++round_robin_;
    return slot;
  }

  // Initializes the orbital constants of `comet` for a start at u0: 70% of
  // the time a near-circular bound orbit (precessing ellipse, immortal),
  // otherwise a low-angular-momentum diver (guaranteed capture). Below the
  // ISCO (r < 6M) the circular branch is unstable, so everything plunges.
  void seed_orbit_geometry(Comet& comet, float u0) noexcept
  {
    const auto potential = [](float u, float l) noexcept {
      return (1.0F - 2.0F * u) * (u * u + l);
    };
    const auto barrier = [&potential](float l) noexcept {
      const float disc = std::max(1.0F - 12.0F * l, 0.0F);
      const float u_star = (1.0F + std::sqrt(disc)) / 6.0F;  // V maximum
      return potential(u_star, l);
    };

    const float l_circ = u0 * (1.0F - 3.0F * u0);
    const float v0_circ = potential(u0, l_circ);
    const bool stable = u0 <= 1.0F / 6.0F;  // outside the ISCO

    float l;
    float e2;
    bool plunger = false;
    if (stable && unit_(rng_) < 0.7F) {
      // Bound: energy between the local potential and the barrier top, so
      // the orbit oscillates around the spawn radius forever. e2 > l lets
      // part of the population slingshot out to the edge (recycled there).
      l = l_circ * (0.9F + 0.2F * unit_(rng_));
      const float v0 = potential(u0, l);
      const float headroom = std::max(barrier(l) - v0, 1e-4F);
      e2 = v0 + eccentricity * headroom * (0.5F + 0.5F * unit_(rng_));
      e2 = std::min(e2, barrier(l) - 1e-4F);
    } else {
      // Plunger: low l lifts nothing, start energy clears the barrier.
      plunger = true;
      l = l_circ * (0.25F + 0.25F * unit_(rng_));
      e2 = std::max(barrier(l), v0_circ) + 0.01F + 0.02F * unit_(rng_);
    }

    comet.u = u0;
    comet.l = l;
    const float w_abs = std::sqrt(std::max(e2 - potential(u0, l), 0.0F));
    comet.w = plunger ? w_abs
                      : (unit_(rng_) < 0.5F ? -w_abs : w_abs);  // in or out
    comet.phi = 6.2831853F * unit_(rng_);

    const float inclination = 0.1F + (max_inclination - 0.1F) * unit_(rng_);
    const float node = 6.2831853F * unit_(rng_);
    const glm::vec3 normal{std::sin(inclination) * std::cos(node),
                           std::sin(inclination) * std::sin(node),
                           std::cos(inclination)};
    comet.e1 = glm::normalize(glm::cross(normal, glm::vec3{0.0F, 0.0F, 1.0F}));
    comet.e2 = glm::cross(normal, comet.e1);
    comet.head = (1.0F / u0) * (std::cos(comet.phi) * comet.e1 +
                                std::sin(comet.phi) * comet.e2);
    comet.trail[0] = comet.head;
    comet.trail_len = 1;
    comet.alive = true;
  }

  // Copies the particle state into the shader uniforms.
  void publish(int index) noexcept
  {
    const auto i = static_cast<std::size_t>(index);
    const Comet& comet = comets[i];
    if (!comet.alive) {
      head_uniforms[i] = {};
      for (int k = 0; k < k_trail; ++k) {
        trail_uniforms[i * k_trail + static_cast<std::size_t>(k)] = {};
      }
      return;
    }
    head_uniforms[i] = {comet.head, 1.0F};
    for (int k = 0; k < k_trail; ++k) {
      auto& slot = trail_uniforms[i * k_trail + static_cast<std::size_t>(k)];
      if (k < comet.trail_len) {
        const float fade =
            1.0F - 0.78F * (static_cast<float>(k) / (k_trail - 1));
        slot = {comet.trail[static_cast<std::size_t>(k)], fade};
      } else {
        slot = {};
      }
    }
  }

  float accumulator_ = 0.0F;
  int round_robin_ = 0;
  std::mt19937 rng_{12345U};
  std::uniform_real_distribution<float> unit_{};
};

// Tidal disruption event: a star dropped on a plunging equatorial orbit,
// spaghettified while falling, torn at r_t into swarm debris plus a disk
// heating flash. Manual trigger (Drop star button / --dropstar). The ball
// glare, the stretch axis and the flash all decay through the shader
// uniforms; the sim keeps the debris alive via the regular swarm slots.
struct StarEvent {
  static constexpr int k_slots = 3;        // concurrent stars (matches shader)
  static constexpr float k_r_tidal = 9.0F;   // disruption radius [M]
  static constexpr float k_r_min = 20.0F;    // drop radius range [M] (random)
  static constexpr float k_r_max = 30.0F;
  static constexpr float k_ball_r = 1.7F;    // ball radius [M] (readable)
  static constexpr int k_debris = 8;
  static constexpr int k_trail = 6;        // history points (matches shader)

  bool active = false;
  float u = 0.0F;
  float w = 0.0F;
  float phi = 0.0F;
  float l = 0.0F;  // squared specific angular momentum (const of motion)
  glm::vec3 e1{1.0F, 0.0F, 0.0F};  // inclined orbital plane basis (world)
  glm::vec3 e2{0.0F, 1.0F, 0.0F};
  glm::vec3 head{};
  glm::vec3 axis{1.0F, 0.0F, 0.0F};  // spaghettification axis (unit)
  std::array<glm::vec3, 6> trail{};  // recent heads, [0] = newest
  int trail_len = 0;
  float stretch = 1.0F;              // 1 = round, grows while falling
  float intensity = 0.0F;            // ball glare (flash on disruption)
  float boost = 0.0F;                // disk heating strength (decays)
  float impact_r = 0.0F;

  void drop(CometSystem& comets, float drop_phi)
  {
    // Plunger recipe (mirrors seed_orbit_geometry plungers): low angular
    // momentum, start energy above the barrier top.
    // drop_phi faces the camera (plus a random +-0.5 rad spread) so the
    // event starts on the visible side, never behind the shadow. Drop
    // radius is random too, so spammed stars stagger naturally.
    // The orbital plane is tilted randomly (0.15..0.9 rad off equatorial):
    // dives cross the disk at an angle instead of sliding inside it.
    const float r0 = k_r_min + (k_r_max - k_r_min) * unit_(rng_);
    u = 1.0F / r0;
    const float l_circ = u * (1.0F - 3.0F * u);
    l = l_circ * 0.3F;
    const float v0 = (1.0F - 2.0F * u) * (u * u + l);
    const float disc = std::max(1.0F - 12.0F * l, 0.0F);
    const float u_star = (1.0F + std::sqrt(disc)) / 6.0F;  // V maximum
    const float barrier = (1.0F - 2.0F * u_star) * (u_star * u_star + l);
    w = std::sqrt(std::max(barrier - v0, 0.0F) + 0.02F);  // inward
    phi = drop_phi + (unit_(rng_) - 0.5F) * 1.0F;
    // In-plane basis: e1 = drop direction (camera side), e2 = normal x e1
    // with the normal tilted off the z-axis for an inclined dive.
    const glm::vec3 radial{std::cos(phi), std::sin(phi), 0.0F};
    const glm::vec3 transverse = glm::normalize(
        glm::cross(glm::vec3{0.0F, 0.0F, 1.0F}, radial));
    const float tilt = 0.15F + 0.75F * unit_(rng_);
    const glm::vec3 normal = std::cos(tilt) * glm::vec3{0.0F, 0.0F, 1.0F} +
                             std::sin(tilt) * transverse;
    e1 = radial - normal * glm::dot(radial, normal);  // radial lies in-plane
    e1 = glm::normalize(e1);
    e2 = glm::cross(normal, e1);
    phi = 0.0F;  // head starts along e1 (camera side)
    head = (1.0F / u) * e1;
    trail[0] = head;
    trail_len = 1;
    axis = {1.0F, 0.0F, 0.0F};
    stretch = 1.0F;
    intensity = 2.0F;  // hot blue-white ball, readable against the beige disk
    boost = 0.0F;
    impact_r = 0.0F;
    active = true;
    comets.running = true;  // a drop starts the clock (never frozen-mysterious)
  }

  void advance(CometSystem& comets) noexcept
  {
    if (!active) {
      intensity *= 0.93F;  // flash + heating decay after disruption (lingering)
      boost *= 0.97F;
      return;
    }
    // One CometSystem-style frame: fixed dphi, 2 RK4 substeps (M = 1).
    // Lightning pacing: the fall lasts ~50 frames (~0.8 s). Independent of
    // the swarm speed slider by design (event clock vs ambient clock).
    constexpr float k_dphi = 0.007F;
    const float h = k_dphi / 2.0F;
    for (int s = 0; s < 2; ++s) {
      // RK4 on (u, w): u' = w, w' = -u + 3u^2 + l (M = 1).
      const auto deriv_w = [](float uu, float ll) noexcept {
        return -uu + 3.0F * uu * uu + ll;
      };
      const float k1u = w;
      const float k1w = deriv_w(u, l);
      const float u2 = u + 0.5F * h * k1u;
      const float w2 = w + 0.5F * h * k1w;
      const float k2u = w2;
      const float k2w = deriv_w(u2, l);
      const float u3 = u + 0.5F * h * k2u;
      const float w3 = w + 0.5F * h * k2w;
      const float k3u = w3;
      const float k3w = deriv_w(u3, l);
      const float u4 = u + h * k3u;
      const float w4 = w + h * k3w;
      const float k4u = w4;
      const float k4w = deriv_w(u4, l);
      u += (h / 6.0F) * (k1u + 2.0F * k2u + 2.0F * k3u + k4u);
      w += (h / 6.0F) * (k1w + 2.0F * k2w + 2.0F * k3w + k4w);
    }
    phi += k_dphi;
    const float r = 1.0F / std::max(u, 1e-6F);
    const glm::vec3 prev = head;
    head = r * (std::cos(phi) * e1 + std::sin(phi) * e2);
    // Push the head onto the trail history (newest first): the luminous
    // streak behind the ball.
    for (int k = k_trail - 1; k > 0; --k) {
      trail[static_cast<std::size_t>(k)] =
          trail[static_cast<std::size_t>(k - 1)];
    }
    trail[0] = head;
    trail_len = std::min(trail_len + 1, k_trail);
    const glm::vec3 motion = head - prev;
    if (glm::length(motion) > 1e-6F) {
      axis = glm::normalize(motion);
    }
    const float q = k_r_tidal / r;
    stretch = std::min(1.0F + 3.0F * q * q, 8.0F);
    if (r <= k_r_tidal) {
      impact_r = r;
      boost = 2.0F;
      intensity = 6.0F;  // white flash (decays above)
      comets.seed_debris(head, k_debris, 0.6F);
      active = false;
    } else if (u >= 0.5F) {
      boost = 0.5F;  // direct-swallow fallback (r_t fires first in practice)
      intensity = 2.0F;
      active = false;
    }
  }

 private:
  std::mt19937 rng_{987U};
  std::uniform_real_distribution<float> unit_{};
};

// Picks a dormant star slot for a new drop, else steals round-robin (the
// oldest event is cut short). Spamming Drop star fills the sky, never
// resets the ones already falling.
[[nodiscard]] StarEvent& take_star_slot(std::array<StarEvent, StarEvent::k_slots>& stars)
{
  for (StarEvent& star : stars) {
    if (!star.active && star.intensity < 0.05F && star.boost < 0.05F) {
      return star;
    }
  }
  static int rr = 0;  // all busy: steal round-robin
  return stars[static_cast<std::size_t>(rr++ % StarEvent::k_slots)];
}

// Spec section 5: Schwarzschild thermodynamics, pure functions of mass (M_sun).
namespace physics {

constexpr double k_g = 6.67430e-11;         // m^3 kg^-1 s^-2
constexpr double k_c = 2.99792458e8;        // m/s
constexpr double k_hbar = 1.054571817e-34;  // J s
constexpr double k_k_b = 1.380649e-23;      // J/K
constexpr double k_m_sun = 1.98892e30;      // kg
constexpr double k_seconds_per_year = 3.15576e7;
constexpr double k_pi = 3.14159265358979323846;

// Schwarzschild radius r_s = 2GM/c^2 (m).
[[nodiscard]] constexpr double schwarzschild_radius(double mass_solar) noexcept
{
  return 2.0 * k_g * (mass_solar * k_m_sun) / (k_c * k_c);
}

// Hawking temperature T = hbar*c^3 / (8*pi*G*M*k_B) (K).
[[nodiscard]] constexpr double hawking_temperature(double mass_solar) noexcept
{
  const double m = mass_solar * k_m_sun;
  return k_hbar * k_c * k_c * k_c / (8.0 * k_pi * k_g * m * k_k_b);
}

// Evaporation time t = 5120*pi*G^2*M^3/(hbar*c^4) (years).
[[nodiscard]] constexpr double evaporation_time_years(double mass_solar) noexcept
{
  const double m = mass_solar * k_m_sun;
  return 5120.0 * k_pi * k_g * k_g * m * m * m /
         (k_hbar * k_c * k_c * k_c * k_c * k_seconds_per_year);
}

// Self-check: one solar mass evaporates in ~2.1e67 years (spec section 5).
static_assert(evaporation_time_years(1.0) > 1.0e67 &&
              evaporation_time_years(1.0) < 3.0e67);

constexpr double k_alpha = 2.011e-4;  // photon-only coefficient (spec 5.2)

// Bekenstein-Hawking entropy S/k_B = A / (4 l_P^2), A = 4 pi r_s^2 (spec 5.1).
[[nodiscard]] constexpr double bh_entropy_over_kb(double mass_solar) noexcept
{
  const double r_s = schwarzschild_radius(mass_solar);
  const double l_p_squared = k_g * k_hbar / (k_c * k_c * k_c);
  return k_pi * r_s * r_s / l_p_squared;
}

// Self-check: solar-mass hole entropy ~ 1e77 k_B.
static_assert(bh_entropy_over_kb(1.0) > 1.0e76 &&
              bh_entropy_over_kb(1.0) < 1.0e78);

// Hawking mass-loss rate dM/dt = -alpha*hbar*c^4/(G^2 M^2) (spec 5.2), kg/s.
[[nodiscard]] constexpr double mass_loss_rate_kg_s(double mass_solar) noexcept
{
  const double m = mass_solar * k_m_sun;
  return -k_alpha * k_hbar * k_c * k_c * k_c * k_c / (k_g * k_g * m * m);
}

// Self-check: solar-mass hole loses ~1e-44 kg/s.
static_assert(mass_loss_rate_kg_s(1.0) < -1.0e-46 &&
              mass_loss_rate_kg_s(1.0) > -1.0e-44);

}  // namespace physics

[[nodiscard]] GLuint compile_shader(GLenum type, const char* source)
{
  const GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);

  GLint status = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
  if (status != GL_TRUE) {
    GLint log_length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
    std::string log(static_cast<std::size_t>(log_length), '\0');
    glGetShaderInfoLog(shader, log_length, nullptr, log.data());
    glDeleteShader(shader);
    throw std::runtime_error{"shader compile failed: " + log};
  }
  return shader;
}

[[nodiscard]] GLuint link_program(GLuint vertex_shader, GLuint fragment_shader)
{
  const GLuint program = glCreateProgram();
  glAttachShader(program, vertex_shader);
  glAttachShader(program, fragment_shader);
  glLinkProgram(program);
  glDeleteShader(vertex_shader);
  glDeleteShader(fragment_shader);

  GLint status = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &status);
  if (status != GL_TRUE) {
    GLint log_length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);
    std::string log(static_cast<std::size_t>(log_length), '\0');
    glGetProgramInfoLog(program, log_length, nullptr, log.data());
    glDeleteProgram(program);
    throw std::runtime_error{"program link failed: " + log};
  }
  return program;
}

// Zeroed comet uniforms for purist mode (sim keeps running, glow hidden).
const std::array<glm::vec4, 16> kZeroCometHeads{};
const std::array<glm::vec4, 128> kZeroCometTrail{};

// Fullscreen Schwarzschild ray-tracing pass (RAII, non-copyable: constructed once).
class RaytracePass {
public:
  RaytracePass()
      : program_{link_program(compile_shader(GL_VERTEX_SHADER, shaders::kRaytraceVertex),
                              compile_shader(GL_FRAGMENT_SHADER, shaders::kRaytraceFragment))}
  {
    glGenVertexArrays(1, &vao_);
    locations_.cam_pos = glGetUniformLocation(program_, "u_cam_pos");
    locations_.cam_fwd = glGetUniformLocation(program_, "u_cam_fwd");
    locations_.cam_right = glGetUniformLocation(program_, "u_cam_right");
    locations_.cam_up = glGetUniformLocation(program_, "u_cam_up");
    locations_.tan_half_fov = glGetUniformLocation(program_, "u_tan_half_fov");
    locations_.aspect = glGetUniformLocation(program_, "u_aspect");
    locations_.mass = glGetUniformLocation(program_, "u_mass");
    locations_.spin = glGetUniformLocation(program_, "u_spin");
    locations_.proj_near = glGetUniformLocation(program_, "u_proj_near");
    locations_.proj_far = glGetUniformLocation(program_, "u_proj_far");
    locations_.disk_inner = glGetUniformLocation(program_, "u_disk_inner");
    locations_.disk_outer = glGetUniformLocation(program_, "u_disk_outer");
    locations_.disk_temperature = glGetUniformLocation(program_, "u_disk_temperature");
    locations_.disk_brightness = glGetUniformLocation(program_, "u_disk_brightness");
    locations_.time = glGetUniformLocation(program_, "u_time");
    locations_.comet_head = glGetUniformLocation(program_, "u_comet_head[0]");
    locations_.comet_trail = glGetUniformLocation(program_, "u_comet_trail[0]");
    locations_.star_pos_radius = glGetUniformLocation(program_, "u_star_pos_radius");
    locations_.star_axis_stretch =
        glGetUniformLocation(program_, "u_star_axis_stretch");
    locations_.star_color = glGetUniformLocation(program_, "u_star_color");
    locations_.star_trail = glGetUniformLocation(program_, "u_star_trail");
    locations_.impact = glGetUniformLocation(program_, "u_impact");
    locations_.resolution = glGetUniformLocation(program_, "u_resolution");
    locations_.pixel = glGetUniformLocation(program_, "u_pixel");
    locations_.realism = glGetUniformLocation(program_, "u_realism");
  }

  ~RaytracePass()
  {
    glDeleteProgram(program_);
    glDeleteVertexArrays(1, &vao_);
  }

  RaytracePass(const RaytracePass&) = delete;
  RaytracePass& operator=(const RaytracePass&) = delete;

  void draw(const Camera& camera, const DiskParams& disk, const CometSystem& comets,
            const std::array<StarEvent, StarEvent::k_slots>& stars, int width,
            int height, float time_seconds, float pixel_size, float spin,
            const glm::vec4& realism) const
  {
    glViewport(0, 0, width, height);
    glUseProgram(program_);

    const glm::vec3 fwd = camera.forward();
    const glm::vec3 right = camera.right();
    const glm::vec3 up = camera.up();
    const float tan_half_fov =
        std::tan(glm::radians(camera.vertical_fov_degrees) * 0.5F);

    glUniform3fv(locations_.cam_pos, 1, glm::value_ptr(camera.position));
    glUniform3fv(locations_.cam_fwd, 1, glm::value_ptr(fwd));
    glUniform3fv(locations_.cam_right, 1, glm::value_ptr(right));
    glUniform3fv(locations_.cam_up, 1, glm::value_ptr(up));
    glUniform1f(locations_.tan_half_fov, tan_half_fov);
    glUniform1f(locations_.aspect, static_cast<float>(width) / static_cast<float>(height));
    glUniform1f(locations_.mass, k_geometric_mass);
    glUniform1f(locations_.spin, spin);
    glUniform1f(locations_.proj_near, k_projection_near);
    glUniform1f(locations_.proj_far, k_projection_far);
    glUniform1f(locations_.disk_inner, disk.inner_radius);
    glUniform1f(locations_.disk_outer, disk.outer_radius);
    glUniform1f(locations_.disk_temperature, disk.peak_temperature);
    glUniform1f(locations_.disk_brightness, disk.brightness);
    glUniform1f(locations_.time, time_seconds);
    glUniform2f(locations_.resolution, static_cast<float>(width),
                static_cast<float>(height));
    glUniform1f(locations_.pixel, pixel_size);
    glUniform4fv(locations_.comet_head, CometSystem::k_count,
                 glm::value_ptr((comets.visible ? comets.head_uniforms
                                                : kZeroCometHeads)[0]));
    glUniform4fv(locations_.comet_trail,
                 CometSystem::k_count * CometSystem::k_trail,
                 glm::value_ptr((comets.visible ? comets.trail_uniforms
                                                : kZeroCometTrail)[0]));
    // TDE star uniforms (ball radius 0 fully disables the shader branch).
    // Trail fades derive from the live intensity, so the streak dies with
    // the flash after disruption.
    glm::vec4 star_pr[StarEvent::k_slots];
    glm::vec4 star_as[StarEvent::k_slots];
    glm::vec4 star_col[StarEvent::k_slots];
    glm::vec4 star_trail[StarEvent::k_slots * StarEvent::k_trail];
    glm::vec2 impact[StarEvent::k_slots];
    for (int s = 0; s < StarEvent::k_slots; ++s) {
      const StarEvent& star = stars[static_cast<std::size_t>(s)];
      const float sr =
          (star.intensity > 0.02F || star.active) ? StarEvent::k_ball_r : 0.0F;
      star_pr[s] = glm::vec4(star.head, sr);
      star_as[s] = glm::vec4(star.axis, star.stretch);
      star_col[s] = glm::vec4(0.8F, 0.9F, 1.0F, star.intensity);  // blue-white
      impact[s] = glm::vec2(star.impact_r, star.boost);
      for (int k = 0; k < StarEvent::k_trail; ++k) {
        float fade = 0.0F;
        if (k < star.trail_len) {
          fade = star.intensity * (1.0F - static_cast<float>(k) /
                                             StarEvent::k_trail);
        }
        star_trail[s * StarEvent::k_trail + k] =
            glm::vec4(star.trail[static_cast<std::size_t>(k)], fade);
      }
    }
    glUniform4fv(locations_.star_pos_radius, StarEvent::k_slots,
                 glm::value_ptr(star_pr[0]));
    glUniform4fv(locations_.star_axis_stretch, StarEvent::k_slots,
                 glm::value_ptr(star_as[0]));
    glUniform4fv(locations_.star_color, StarEvent::k_slots,
                 glm::value_ptr(star_col[0]));
    glUniform4fv(locations_.star_trail,
                 StarEvent::k_slots * StarEvent::k_trail,
                 glm::value_ptr(star_trail[0]));
    glUniform2fv(locations_.impact, StarEvent::k_slots,
                 glm::value_ptr(impact[0]));
    glUniform4f(locations_.realism, realism.x, realism.y, realism.z, realism.w);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }

private:
  struct UniformLocations {
    GLint cam_pos = -1;
    GLint cam_fwd = -1;
    GLint cam_right = -1;
    GLint cam_up = -1;
    GLint tan_half_fov = -1;
    GLint aspect = -1;
    GLint mass = -1;
    GLint spin = -1;
    GLint proj_near = -1;
    GLint proj_far = -1;
    GLint disk_inner = -1;
    GLint disk_outer = -1;
    GLint realism = -1;
    GLint disk_temperature = -1;
    GLint disk_brightness = -1;
    GLint time = -1;
    GLint comet_head = -1;
    GLint comet_trail = -1;
    GLint star_pos_radius = -1;
    GLint star_axis_stretch = -1;
    GLint star_color = -1;
    GLint star_trail = -1;
    GLint impact = -1;
    GLint resolution = -1;
    GLint pixel = -1;
  };

  GLuint program_ = 0;
  GLuint vao_ = 0;
  UniformLocations locations_{};
};

// Rasterized spacetime grid, depth-tested against the ray-traced buffer.
class GridPass {
public:
  GridPass()
      : program_{link_program(compile_shader(GL_VERTEX_SHADER, shaders::kGridVertex),
                              compile_shader(GL_FRAGMENT_SHADER, shaders::kGridFragment))}
  {
    const std::vector<glm::vec3> vertices = build_vertices();
    vertex_count_ = static_cast<GLsizei>(vertices.size());

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertices.size() * sizeof(glm::vec3)),
                 vertices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
    glBindVertexArray(0);

    locations_.mvp = glGetUniformLocation(program_, "u_mvp");
    locations_.mass = glGetUniformLocation(program_, "u_mass");
    locations_.max_radius = glGetUniformLocation(program_, "u_grid_max_radius");
    locations_.resolution = glGetUniformLocation(program_, "u_resolution");
    locations_.pixel = glGetUniformLocation(program_, "u_pixel");
  }

  ~GridPass()
  {
    glDeleteProgram(program_);
    glDeleteVertexArrays(1, &vao_);
    glDeleteBuffers(1, &vbo_);
  }

  GridPass(const GridPass&) = delete;
  GridPass& operator=(const GridPass&) = delete;

  void draw(const glm::mat4& mvp, int width, int height, float pixel_size,
            float mass) const
  {
    glUseProgram(program_);
    glUniformMatrix4fv(locations_.mvp, 1, GL_FALSE, glm::value_ptr(mvp));
    glUniform1f(locations_.mass, mass);
    glUniform1f(locations_.max_radius, k_grid_max_radius);
    glUniform2f(locations_.resolution, static_cast<float>(width),
                static_cast<float>(height));
    glUniform1f(locations_.pixel, pixel_size);
    glBindVertexArray(vao_);
    glDrawArrays(GL_LINES, 0, vertex_count_);
  }

private:
  // Grid of GL_LINES: each line subdivided so the vertex warp can curve it
  // through the gravity funnel instead of cutting a chord across it.
  [[nodiscard]] static std::vector<glm::vec3> build_vertices()
  {
    std::vector<glm::vec3> vertices;
    vertices.reserve(static_cast<std::size_t>(k_grid_line_count) *
                     k_grid_segment_count * 4);

    const float extent = k_grid_max_radius;
    const float line_step =
        2.0F * extent / static_cast<float>(k_grid_line_count - 1);
    const float segment_step =
        2.0F * extent / static_cast<float>(k_grid_segment_count);

    for (int line = 0; line < k_grid_line_count; ++line) {
      const float coord = -extent + static_cast<float>(line) * line_step;
      for (int segment = 0; segment < k_grid_segment_count; ++segment) {
        const float t0 = -extent + static_cast<float>(segment) * segment_step;
        const float t1 = t0 + segment_step;
        vertices.emplace_back(t0, coord, 0.0F);
        vertices.emplace_back(t1, coord, 0.0F);
        vertices.emplace_back(coord, t0, 0.0F);
        vertices.emplace_back(coord, t1, 0.0F);
      }
    }
    return vertices;
  }

  struct UniformLocations {
    GLint mvp = -1;
    GLint mass = -1;
    GLint max_radius = -1;
    GLint resolution = -1;
    GLint pixel = -1;
  };

  GLuint program_ = 0;
  GLuint vao_ = 0;
  GLuint vbo_ = 0;
  GLsizei vertex_count_ = 0;
  UniformLocations locations_{};
};

// --polish: scene rendered offscreen, bright-pass + blur at quarter res,
// composite back (bloom + FXAA). Reallocates only when the window resizes.
class PostChain {
public:
  PostChain()
      : blur_program_{link_program(compile_shader(GL_VERTEX_SHADER, shaders::kRaytraceVertex),
                                   compile_shader(GL_FRAGMENT_SHADER, shaders::kBlurFragment))},
        composite_program_{link_program(compile_shader(GL_VERTEX_SHADER, shaders::kRaytraceVertex),
                                        compile_shader(GL_FRAGMENT_SHADER, shaders::kCompositeFragment))}
  {
    blur_scene_ = glGetUniformLocation(blur_program_, "u_scene");
    blur_texel_ = glGetUniformLocation(blur_program_, "u_texel");
    blur_dir_ = glGetUniformLocation(blur_program_, "u_dir");
    blur_threshold_ = glGetUniformLocation(blur_program_, "u_threshold");
    comp_scene_ = glGetUniformLocation(composite_program_, "u_scene");
    comp_bloom_ = glGetUniformLocation(composite_program_, "u_bloom");
    comp_strength_ = glGetUniformLocation(composite_program_, "u_bloom_strength");
    comp_resolution_ = glGetUniformLocation(composite_program_, "u_resolution");
    comp_pixel_ = glGetUniformLocation(composite_program_, "u_pixel");
    glGenVertexArrays(1, &vao_);
  }

  ~PostChain() { shutdown(); }

  PostChain(const PostChain&) = delete;
  PostChain& operator=(const PostChain&) = delete;

  GLuint scene_fbo(int width, int height)
  {
    if (scene_fbo_ != 0 && (width_ != width || height_ != height)) {
      glDeleteFramebuffers(1, &scene_fbo_);
      glDeleteTextures(1, &scene_color_);
      glDeleteRenderbuffers(1, &scene_depth_);
      glDeleteFramebuffers(1, &fbo_a_);
      glDeleteTextures(1, &tex_a_);
      glDeleteFramebuffers(1, &fbo_b_);
      glDeleteTextures(1, &tex_b_);
      scene_fbo_ = 0;
    }
    if (scene_fbo_ == 0) {
      create(width, height);
    }
    return scene_fbo_;
  }

  // Blur passes then composite onto the currently bound default framebuffer.
  // The scene may be smaller than the output (render scale): texture-space
  // math uses the scene size, the composite covers the full output (the
  // texture sampler upscales for free, FXAA runs at output resolution).
  void render(int scene_w, int scene_h, int out_w, int out_h,
              float pixel_size, float bloom_strength)
  {
    scene_fbo(scene_w, scene_h);
    const int qw = scene_w / 4 > 0 ? scene_w / 4 : 1;
    const int qh = scene_h / 4 > 0 ? scene_h / 4 : 1;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    // Pass 1: full-res scene -> quarter-res bright-pass + blur H.
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_a_);
    glViewport(0, 0, qw, qh);
    glUseProgram(blur_program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_color_);
    glUniform1i(blur_scene_, 0);
    glUniform2f(blur_texel_, 1.0F / static_cast<float>(scene_w),
                1.0F / static_cast<float>(scene_h));
    glUniform2f(blur_dir_, 1.0F, 0.0F);
    glUniform1f(blur_threshold_, 0.35F);
    draw_fullscreen();

    // Pass 2: quarter-res blur V.
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_b_);
    glViewport(0, 0, qw, qh);
    glBindTexture(GL_TEXTURE_2D, tex_a_);
    glUniform2f(blur_texel_, 1.0F / static_cast<float>(qw),
                1.0F / static_cast<float>(qh));
    glUniform2f(blur_dir_, 0.0F, 1.0F);
    glUniform1f(blur_threshold_, 0.0F);
    draw_fullscreen();

    // Composite onto the backbuffer.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, out_w, out_h);
    glUseProgram(composite_program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_color_);
    glUniform1i(comp_scene_, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex_b_);
    glUniform1i(comp_bloom_, 1);
    glActiveTexture(GL_TEXTURE0);
    glUniform1f(comp_strength_, bloom_strength);
    glUniform2f(comp_resolution_, static_cast<float>(out_w),
                static_cast<float>(out_h));
    glUniform1f(comp_pixel_, pixel_size);
    // Color only: the backbuffer depth is filled separately (depth blit),
    // otherwise the fullscreen quad would poison it with a flat value and
    // the crisp grid drawn afterwards would depth-test against garbage.
    glDepthMask(GL_FALSE);
    draw_fullscreen();
    glDepthMask(GL_TRUE);
  }

  // Fast path without polish: blit the scaled scene (color + depth) onto the
  // backbuffer. Color upscales smooth (LINEAR) or pixelated (NEAREST, retro),
  // depth always with NEAREST so later full-res work depth-tests correctly.
  void blit_to_screen(int out_w, int out_h, bool pixelated)
  {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, scene_fbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, width_, height_, 0, 0, out_w, out_h,
                      GL_COLOR_BUFFER_BIT,
                      pixelated ? GL_NEAREST : GL_LINEAR);
    glBlitFramebuffer(0, 0, width_, height_, 0, 0, out_w, out_h,
                      GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }

  // Depth only (used after the polish composite, which outputs color): lets
  // a later full-res pass depth-test against the scaled scene.
  void blit_depth_to_screen(int out_w, int out_h)
  {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, scene_fbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, width_, height_, 0, 0, out_w, out_h,
                      GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }

private:
  void create(int width, int height)
  {
    width_ = width;
    height_ = height;
    const int qw = width_ / 4 > 0 ? width_ / 4 : 1;
    const int qh = height_ / 4 > 0 ? height_ / 4 : 1;

    glGenFramebuffers(1, &scene_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo_);
    glGenTextures(1, &scene_color_);
    glBindTexture(GL_TEXTURE_2D, scene_color_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           scene_color_, 0);
    glGenRenderbuffers(1, &scene_depth_);
    glBindRenderbuffer(GL_RENDERBUFFER, scene_depth_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width_, height_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, scene_depth_);

    auto make_quarter = [](GLuint& fbo, GLuint& tex, int w, int h) {
      glGenFramebuffers(1, &fbo);
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glGenTextures(1, &tex);
      glBindTexture(GL_TEXTURE_2D, tex);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                             tex, 0);
    };
    make_quarter(fbo_a_, tex_a_, qw, qh);
    make_quarter(fbo_b_, tex_b_, qw, qh);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }

  void shutdown()
  {
    if (scene_fbo_ != 0) {
      glDeleteFramebuffers(1, &scene_fbo_);
      glDeleteTextures(1, &scene_color_);
      glDeleteRenderbuffers(1, &scene_depth_);
      glDeleteFramebuffers(1, &fbo_a_);
      glDeleteTextures(1, &tex_a_);
      glDeleteFramebuffers(1, &fbo_b_);
      glDeleteTextures(1, &tex_b_);
      scene_fbo_ = 0;
    }
    glDeleteProgram(blur_program_);
    glDeleteProgram(composite_program_);
    glDeleteVertexArrays(1, &vao_);
    blur_program_ = composite_program_ = 0;
    vao_ = 0;
  }

  void draw_fullscreen()
  {
    // Raytrace vertex shader builds its triangle from gl_VertexID: empty VAO.
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }

  GLuint blur_program_ = 0;
  GLuint composite_program_ = 0;
  GLuint vao_ = 0;
  GLuint scene_fbo_ = 0, scene_color_ = 0, scene_depth_ = 0;
  GLuint fbo_a_ = 0, tex_a_ = 0, fbo_b_ = 0, tex_b_ = 0;
  int width_ = 0, height_ = 0;
  GLint blur_scene_ = -1, blur_texel_ = -1, blur_dir_ = -1, blur_threshold_ = -1;
  GLint comp_scene_ = -1, comp_bloom_ = -1, comp_strength_ = -1,
        comp_resolution_ = -1, comp_pixel_ = -1;
};

struct RunOptions {
  int frame_limit = -1;             // -1 = run until quit
  std::string screenshot_path;      // empty = no screenshot
  std::optional<float> azimuth_degrees;  // camera orbit override for testing
  std::optional<float> orbit_distance;   // camera distance [M] for testing
  std::optional<float> orbit_speed;      // auto-orbit rad/s (0 = static)
  bool animate = false;             // start the event clock immediately
  int drop_star_count = 0;          // TDE stars dropped at startup (0 = none)
  float pixel_size = 1.0F;          // retro block size (1 = native)
  bool auto_quality = false;      // step render scale to hold ~60 fps
  float render_scale = 1.0F;      // raytrace/grid resolution factor (0.25..1)
  bool upscale_nearest = false;   // pixelated (nearest) vs smooth upscale
  bool fullscreen = false;          // borderless fullscreen at startup
  bool polish = false;              // bloom + FXAA post chain
  float mass_solar = 1.0F;          // section-5 mass slider override
  float spin = 0.0F;                // dimensionless Kerr spin a* [-0.95, 0.95]
};

// Parses CLI flags; exits via throw on bad input (E.2).
[[nodiscard]] RunOptions parse_options(int argc, char* argv[])
{
  RunOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    if (arg == "--frames" && i + 1 < argc) {
      options.frame_limit = std::atoi(argv[++i]);
    } else if (arg == "--screenshot" && i + 1 < argc) {
      options.screenshot_path = argv[++i];
    } else if (arg == "--az" && i + 1 < argc) {
      options.azimuth_degrees = std::strtof(argv[++i], nullptr);
    } else if (arg == "--dist" && i + 1 < argc) {
      options.orbit_distance = std::strtof(argv[++i], nullptr);
    } else if (arg == "--orbit" && i + 1 < argc) {
      options.orbit_speed = std::strtof(argv[++i], nullptr);
    } else if (arg == "--anim") {
      options.animate = true;
    } else if (arg == "--dropstar") {
      options.drop_star_count = 1;
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        options.drop_star_count =
            std::clamp(std::atoi(argv[++i]), 1, StarEvent::k_slots);
      }
    } else if (arg == "--pixel") {
      options.pixel_size = 4.0F;
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        options.pixel_size = std::strtof(argv[++i], nullptr);
      }
    } else if (arg == "--autoq") {
      options.auto_quality = true;
    } else if (arg == "--scale" && i + 1 < argc) {
      options.render_scale =
          std::clamp(std::strtof(argv[++i], nullptr), 0.1F, 1.0F);
    } else if (arg == "--pixup") {
      options.upscale_nearest = true;
    } else if (arg == "--fullscreen") {
      options.fullscreen = true;
    } else if (arg == "--polish") {
      options.polish = true;
    } else if (arg == "--mass" && i + 1 < argc) {
      options.mass_solar = std::strtof(argv[++i], nullptr);
    } else if (arg == "--spin" && i + 1 < argc) {
      options.spin = std::strtof(argv[++i], nullptr);
    } else {
      throw std::invalid_argument{
          "usage: blackhole [--frames N] [--screenshot file.bmp] "
          "[--az degrees] [--dist M] [--orbit rad/s] [--anim] [--dropstar [N]] "
          "[--pixel [N]] [--autoq] [--scale f] [--pixup] [--fullscreen] [--polish] [--mass M_sun] [--spin A]"};
    }
  }
  if (!options.screenshot_path.empty() && options.frame_limit < 0) {
    throw std::invalid_argument{"--screenshot requires --frames N"};
  }
  return options;
}

// Captures the back buffer and saves it as a BMP (rows flipped: GL is bottom-up).
void save_screenshot(int width, int height, const std::string& path)
{
  // A fresh checkout / zip may not have the target folder yet (Windows).
  const std::filesystem::path parent = std::filesystem::path{path}.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }
  const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
  std::vector<std::uint8_t> pixels(row_bytes * static_cast<std::size_t>(height));
  glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

  SDL_Surface* surface =
      SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
  if (!surface) {
    throw std::runtime_error{"SDL_CreateRGBSurfaceWithFormat failed: " + std::string{SDL_GetError()}};
  }

  auto* destination = static_cast<std::uint8_t*>(surface->pixels);
  for (int row = 0; row < height; ++row) {
    const std::size_t source_offset = row_bytes * static_cast<std::size_t>(height - 1 - row);
    std::memcpy(destination + row_bytes * static_cast<std::size_t>(row),
                pixels.data() + source_offset, row_bytes);
  }

  const int status = SDL_SaveBMP(surface, path.c_str());
  SDL_FreeSurface(surface);
  if (status != 0) {
    throw std::runtime_error{"SDL_SaveBMP failed: " + std::string{SDL_GetError()}};
  }
  std::cout << "screenshot saved: " << path << '\n';
}

void request_gl_profile()
{
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, kGlMajorVersion);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, kGlMinorVersion);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
}

void init_glew()
{
  glewExperimental = GL_TRUE;
  const GLenum status = glewInit();
  if (status != GLEW_OK) {
    throw std::runtime_error{"glewInit failed: " +
                             std::string{reinterpret_cast<const char*>(glewGetErrorString(status))}};
  }
  glGetError();  // glewInit may raise a benign GL_INVALID_ENUM on core profiles
}

void print_gl_info()
{
  const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
  const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
  std::cout << "OpenGL " << (version ? version : "?") << " on " << (renderer ? renderer : "?") << '\n';
  GLint depth_attachment = 0;
  glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH,
                                        GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE,
                                        &depth_attachment);
  std::cout << "depth attachment size: " << depth_attachment << '\n';
}

// Rendering prefs (retro block size, polish chain) — shared by CLI and UI.
struct RenderPrefs {
  float pixel_size = 1.0F;
  bool auto_quality = false;  // step render scale to hold ~60 fps
  float render_scale = 1.0F;  // raytrace/grid resolution factor (0.1..1)
  int auto_scale_idx = 0;     // index into kAutoScales while auto is on
  bool upscale_nearest = false;  // pixelated (nearest) vs smooth upscale
  bool fullscreen = false;       // borderless fullscreen (F11)
  bool physical_colors = false;  // true NT Kelvin (blue-hot) vs stylized beige
  bool lock_meters = true;       // camera fixed in physical meters: mass visibly rescales the scene
  bool show_grid = true;
  bool polish = false;
  float bloom_strength = 0.5F;
  bool realism_plunge = true;
  bool realism_jets = true;
  bool realism_ergo = true;
  bool realism_turbulence = true;
};

// Novikov-Thorne scaling at fixed radiative efficiency: T^4 ~ Mdot/M^2.
// Written with the Eddington ratio mdot = Mdot/Mdot_Edd (Mdot_Edd ~ M), so
// T_peak = 5200 K * mdot^1/4 * M_sun^-1/4, normalized to the default look
// (5200 K at 1 M_sun, Eddington). Fully derived: no manual override.
[[nodiscard]] float peak_temp_for_mass(float mass_solar, float eddington)
{
  const float m = std::clamp(mass_solar, 0.1F, 100.0F);
  const float e = std::clamp(eddington, 0.01F, 1.0F);
  return std::clamp(5200.0F * std::pow(e, 0.25F) * std::pow(m, -0.25F),
                    1000.0F, 15000.0F);
}

// True Novikov-Thorne scale (physical-colors mode): peak color temperature
// ~1.4e7 K for a 10 M_sun hole at Eddington, T ~ M^-1/4 mdot^1/4. Blue-hot
// instead of the stylized beige default.
[[nodiscard]] float nt_peak_temp(float mass_solar, float eddington)
{
  const float m = std::clamp(mass_solar, 0.1F, 100.0F);
  const float e = std::clamp(eddington, 0.01F, 1.0F);
  return 1.4e7F * std::pow(m / 10.0F, -0.25F) * std::pow(e, 0.25F);
}

// Render-scale steps driven by the auto quality scaler: raytrace/grid work
// scales with the square, so 0.5x costs ~1/4 and 0.15x ~1/44 of full res
// (kavan-style pixelation, Kerr physics untouched).
inline constexpr float kAutoScales[] = {1.0F,  0.8F, 0.65F, 0.5F, 0.4F,
                                        0.3F, 0.25F, 0.2F, 0.15F, 0.12F,
                                        0.1F};
inline constexpr int kAutoScaleCount = 11;

[[nodiscard]] int nearest_scale_index(float scale) noexcept
{
  int best = 0;
  for (int i = 1; i < kAutoScaleCount; ++i) {
    if (std::fabs(kAutoScales[i] - scale) < std::fabs(kAutoScales[best] - scale)) {
      best = i;
    }
  }
  return best;
}

// English control panel (spec section 3): camera, disk, animation, thermodynamics.
void draw_settings_ui(OrbitState& orbit, DiskParams& disk, float& mass_solar,
                       CometSystem& comets, RenderPrefs& render_prefs,
                       float& spin,
                       std::array<StarEvent, StarEvent::k_slots>& stars)
{
  const float azimuth_max = static_cast<float>(physics::k_pi);

  // Auto-fit height: a stale saved size used to clip the animation section.
  ImGui::SetNextWindowSize(ImVec2{380.0F, 0.0F}, ImGuiCond_Appearing);
  ImGui::Begin("Settings");
  ImGui::TextUnformatted("Camera");
  ImGui::SliderFloat("Azimuth", &orbit.azimuth, -azimuth_max, azimuth_max, "%.2f rad");
  ImGui::SliderFloat("Elevation", &orbit.elevation, -k_max_elevation,
                     k_max_elevation, "%.2f rad");
  ImGui::SliderFloat("Distance", &orbit.distance, k_min_orbit_distance,
                     k_max_orbit_distance);
  // Physical readout: with the meters lock on, THIS is what stays fixed
  // while the mass slider moves (1477 m per solar mass per M unit).
  ImGui::TextDisabled("= %.3g m physical",
                      (double)(orbit.distance * mass_solar * 1477.0F));
  ImGui::SliderFloat("Field of view", &orbit.vertical_fov_degrees, 25.0F, 100.0F);
  ImGui::Checkbox("Auto-orbit", &orbit.auto_orbit);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-1.0F);
  ImGui::SliderFloat("##orbit_speed", &orbit.orbit_speed, -0.3F, 0.3F,
                     "Speed %+.2f rad/s");
  if (ImGui::Button("Side")) {
    orbit.azimuth = -1.5707963F;
    orbit.elevation = 0.12F;
  }
  ImGui::SameLine();
  if (ImGui::Button("45 deg")) {
    orbit.azimuth = -1.5707963F;
    orbit.elevation = 0.7853982F;
  }
  ImGui::SameLine();
  if (ImGui::Button("Top")) {
    orbit.azimuth = -1.5707963F;
    orbit.elevation = 1.45F;
  }
  ImGui::Separator();
  ImGui::TextUnformatted("Accretion disk");
  ImGui::SliderFloat("Spin a*", &spin, -0.95F, 0.95F, "%.2f");
  // Strict maths: the inner edge IS the Kerr ISCO (spec 2.2) — derived from
  // the spin every frame, no manual override.
  disk.inner_radius = kerr_isco(spin);
  ImGui::TextDisabled("Inner radius: %.2f M (ISCO)", disk.inner_radius);
  // Outer edge: free modeling choice (the spec fixes no r_out), shown as a
  // multiple of the ISCO so the proportion stays readable.
  ImGui::SliderFloat("Outer radius", &disk.outer_radius,
                     disk.inner_radius + 1.0F, 40.0F, "%.1f M");
  ImGui::TextDisabled("= x%.2f ISCO", disk.outer_radius / kerr_isco(spin));
  // Strict maths: Novikov-Thorne peak temperature derived from the mass and
  // the Eddington ratio — both free parameters, the temperature never is.
  ImGui::SliderFloat("Eddington ratio", &disk.eddington_ratio, 0.01F, 1.0F,
                     "%.2f", ImGuiSliderFlags_Logarithmic);
  ImGui::Checkbox("Physical colors (true Kelvin, blue-hot)",
                  &render_prefs.physical_colors);
  disk.peak_temperature = render_prefs.physical_colors
                              ? nt_peak_temp(mass_solar, disk.eddington_ratio)
                              : peak_temp_for_mass(mass_solar,
                                                   disk.eddington_ratio);
  ImGui::TextDisabled("Peak temperature: %.3g K (%s)", disk.peak_temperature,
                      render_prefs.physical_colors ? "true NT scale"
                                                   : "stylized");
  ImGui::SliderFloat("Brightness", &disk.brightness, 0.1F, 5.0F);
  ImGui::Separator();
  ImGui::TextUnformatted("Tidal disruption");
  if (ImGui::Button(comets.running ? "Pause" : "Play")) {
    comets.running = !comets.running;
  }
  ImGui::SameLine();
  // Drop a star on a plunging orbit; it stretches while falling, tears at
  // 9M into debris and flashes the disk. Debris lives in the particle
  // slots (invisible backend): no ambient swarm, no GPU cost at rest.
  // Spamming fills every slot; a full sky steals the oldest event.
  if (ImGui::Button("Drop star")) {
    take_star_slot(stars).drop(comets, orbit.azimuth);
  }
  ImGui::SameLine();
  int falling = 0;
  float nearest = 0.0F;
  bool heating = false;
  for (const StarEvent& star : stars) {
    if (star.active) {
      ++falling;
      const float r = 1.0F / std::max(star.u, 1e-6F);
      nearest = falling == 1 ? r : std::min(nearest, r);
    }
    heating = heating || star.boost > 0.02F;
  }
  if (falling > 0) {
    ImGui::Text("%dx falling r~%.1fM", falling, nearest);
  } else if (heating) {
    ImGui::Text("disrupted! %d debris", comets.alive_count());
  } else {
    ImGui::TextDisabled("no star");
  }
  ImGui::Separator();
  ImGui::TextUnformatted("Rendering");
  const ImGuiIO& render_io = ImGui::GetIO();
  if (render_io.Framerate > 0.0F) {
    ImGui::Text("%.1f ms  (%.0f fps)", 1000.0F / render_io.Framerate,
                render_io.Framerate);
  }
  ImGui::SliderFloat("Pixel size", &render_prefs.pixel_size, 1.0F, 8.0F,
                     "%.0f px");
  if (ImGui::IsItemEdited()) {
    render_prefs.auto_quality = false;  // manual override wins
  }
  // Render scale drives raytrace/grid resolution (and fps); pixel size is
  // a retro look only.
  float scale_pct = render_prefs.render_scale * 100.0F;
  if (ImGui::SliderFloat("Render scale", &scale_pct, 10.0F, 100.0F, "%.0f%%")) {
    render_prefs.render_scale = scale_pct / 100.0F;
    render_prefs.auto_quality = false;  // manual override wins
  }
  ImGui::Checkbox("Pixelated upscale (nearest, kavan-style)",
                  &render_prefs.upscale_nearest);
  ImGui::Checkbox("Fullscreen (F11)", &render_prefs.fullscreen);
  ImGui::Checkbox("Show grid", &render_prefs.show_grid);
  if (ImGui::Checkbox("Auto quality (hold ~60 fps)",
                      &render_prefs.auto_quality) &&
      render_prefs.auto_quality) {
    // Resume the auto walk from the current scale.
    render_prefs.auto_scale_idx = nearest_scale_index(render_prefs.render_scale);
  }
  if (render_prefs.auto_quality) {
    ImGui::TextDisabled("auto scale: %.0f%%",
                        kAutoScales[render_prefs.auto_scale_idx] * 100.0F);
  }
  ImGui::Checkbox("Polish (bloom + FXAA)", &render_prefs.polish);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-1.0F);
  if (!render_prefs.polish) {
    ImGui::BeginDisabled();  // bloom strength is dead without the chain
  }
  ImGui::SliderFloat("##bloom", &render_prefs.bloom_strength, 0.0F, 1.5F,
                     "Bloom x%.2f");
  if (!render_prefs.polish) {
    ImGui::EndDisabled();
  }
  ImGui::Separator();
  ImGui::TextUnformatted("Realism");
  if (ImGui::Button("Purist view (physics only)")) {
    // One-way preset: rays, disk, starfield stay; decorative jets, ergosphere
    // glow, swarm streaks and the stylized grid go. Re-enable each below.
    render_prefs.realism_jets = false;
    render_prefs.realism_ergo = false;
    render_prefs.show_grid = false;
    comets.visible = false;
  }
  ImGui::Checkbox("Plunging region", &render_prefs.realism_plunge);
  ImGui::Checkbox("Polar jets", &render_prefs.realism_jets);
  ImGui::Checkbox("Ergosphere glow", &render_prefs.realism_ergo);
  ImGui::Checkbox("Disk turbulence", &render_prefs.realism_turbulence);
  ImGui::End();

  // Right column: keep clear of the Settings window on first use.
  const ImGuiIO& io = ImGui::GetIO();
  // Previous mass for the meters-locked camera rescale (single call site).
  static float last_mass_solar = -1.0F;
  if (last_mass_solar < 0.0F) {
    last_mass_solar = mass_solar;  // init (honors --mass)
  }
  ImGui::SetNextWindowPos(ImVec2{io.DisplaySize.x - 320.0F, 60.0F},
                          ImGuiCond_Appearing);
  ImGui::SetNextWindowSize(ImVec2{300.0F, 0.0F}, ImGuiCond_Appearing);
  ImGui::Begin("Thermodynamics (spec section 5)");
  if (ImGui::SliderFloat("Mass", &mass_solar, 0.1F, 100.0F, "%.1f M_sun")) {
    // Camera locked in physical meters: hold the physical distance fixed, so
    // a heavier hole visibly grows on screen (same physics, nearer in M).
    // distance_M_new = distance_M_old * M_old / M_new.
    if (render_prefs.lock_meters && last_mass_solar > 0.0F) {
      orbit.distance =
          std::clamp(orbit.distance * last_mass_solar / mass_solar,
                     k_min_orbit_distance, k_max_orbit_distance);
    }
  }
  last_mass_solar = mass_solar;
  ImGui::Checkbox("Camera locked in physical meters",
                  &render_prefs.lock_meters);
  ImGui::Text("Schwarzschild radius: %.5g m",
              physics::schwarzschild_radius(mass_solar));
  ImGui::Text("Photon sphere: %.5g m",
              1.5 * physics::schwarzschild_radius(mass_solar));
  const float rs = physics::schwarzschild_radius(mass_solar);
  const float r_plus = 0.5F * rs * (1.0F + std::sqrt(std::max(1.0F - spin * spin, 0.0F)));
  ImGui::Text("Horizon r+: %.5g m", r_plus);
  ImGui::Text("Ergoregion (eq.): %.5g m", rs - r_plus);
  ImGui::Text("ISCO: %.5g m", 0.5F * rs * kerr_isco(spin));
  ImGui::Text("Hawking temperature: %.5g K",
              physics::hawking_temperature(mass_solar));
  ImGui::Text("Entropy: %.5g k_B", physics::bh_entropy_over_kb(mass_solar));
  ImGui::Text("dM/dt: %.5g kg/s", physics::mass_loss_rate_kg_s(mass_solar));
  ImGui::Text("Evaporation time: %.5g years",
              physics::evaporation_time_years(mass_solar));
  ImGui::TextDisabled("Geometric units (M = 1) are scale-invariant: with the");
  ImGui::TextDisabled("camera locked in meters, a heavier hole looks bigger;");
  ImGui::TextDisabled("mass also drives thermo rows, grid depth, clocks, T.");
  ImGui::End();
}

}  // namespace

int main(int argc, char* argv[])
{
  try {
    RunOptions options = parse_options(argc, argv);
#ifdef BLACKHOLE_DEFAULT_ARGS
    // Build variant with baked-in flags (e.g. --polish --anim): used only
    // when the user passes no arguments on the command line.
    if (argc <= 1) {
      std::istringstream preset{BLACKHOLE_DEFAULT_ARGS};
      std::vector<std::string> args{"blackhole"};
      std::string token;
      while (preset >> token) {
        args.push_back(token);
      }
      std::vector<char*> argv_preset;
      argv_preset.reserve(args.size());
      for (std::string& argument : args) {
        argv_preset.push_back(argument.data());
      }
      options = parse_options(static_cast<int>(argv_preset.size()),
                              argv_preset.data());
    }
#endif

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
      throw std::runtime_error{"SDL_Init failed: " + std::string{SDL_GetError()}};
    }
    // sdl2-compat drops GL attributes set before the GL driver is loaded.
    if (SDL_GL_LoadLibrary(nullptr) != 0) {
      throw std::runtime_error{"SDL_GL_LoadLibrary failed: " + std::string{SDL_GetError()}};
    }
    request_gl_profile();

    WindowPtr window{SDL_CreateWindow("Space++ - Black Hole Simulator",
                                      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                      kWindowWidth, kWindowHeight,
                                      SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE)};
    if (!window) {
      throw std::runtime_error{"SDL_CreateWindow failed: " + std::string{SDL_GetError()}};
    }

    GlContext context{*window};
    if (SDL_GL_MakeCurrent(window.get(), context.handle()) != 0) {
      throw std::runtime_error{"SDL_GL_MakeCurrent failed: " + std::string{SDL_GetError()}};
    }
    SDL_GL_SetSwapInterval(1);

    init_glew();
    print_gl_info();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(window.get(), context.handle());
    ImGui_ImplOpenGL3_Init("#version 460");

    const RaytracePass raytrace_pass;
    const GridPass grid_pass;
    PostChain post_chain;
    RenderPrefs render_prefs;
    render_prefs.pixel_size = glm::max(options.pixel_size, 1.0F);
    render_prefs.auto_quality = options.auto_quality;
    render_prefs.render_scale =
        std::clamp(options.render_scale, 0.1F, 1.0F);
    render_prefs.upscale_nearest = options.upscale_nearest;
    render_prefs.fullscreen = options.fullscreen;
    render_prefs.auto_scale_idx = nearest_scale_index(render_prefs.render_scale);
    render_prefs.polish = options.polish;
    DiskParams disk;
    disk.peak_temperature =
        peak_temp_for_mass(options.mass_solar, disk.eddington_ratio);
    float hud_mass_solar = options.mass_solar;
    float hud_spin = options.spin;
    disk.inner_radius = kerr_isco(std::clamp(hud_spin, -0.95F, 0.95F));
    CometSystem comets;
    std::array<StarEvent, StarEvent::k_slots> stars;
    if (options.animate) {
      comets.running = true;
    }
    OrbitState orbit;
    if (options.azimuth_degrees.has_value()) {
      orbit.azimuth = glm::radians(*options.azimuth_degrees);
    }
    if (options.orbit_distance.has_value()) {
      orbit.distance =
          std::clamp(*options.orbit_distance, k_min_orbit_distance,
                     k_max_orbit_distance);
    }
    if (options.orbit_speed.has_value()) {
      orbit.orbit_speed = *options.orbit_speed;
      orbit.auto_orbit = *options.orbit_speed != 0.0F;
    }
    if (render_prefs.lock_meters && options.mass_solar != 1.0F) {
      // Startup version of the slider rule: hold physical distance, so a
      // heavier hole already fills more of the first frame.
      orbit.distance =
          std::clamp(orbit.distance / options.mass_solar,
                     k_min_orbit_distance, k_max_orbit_distance);
    }
    if (options.drop_star_count > 0) {
      for (int i = 0; i < options.drop_star_count; ++i) {
        take_star_slot(stars).drop(comets, orbit.azimuth);
      }
    }

    bool dragging = false;
    bool running = true;
    int frame = 0;
    // Auto quality scaler state: EMA frame time + cooldown/calm counters.
    const Uint64 tick_freq = SDL_GetPerformanceFrequency();
    Uint64 last_ticks = SDL_GetPerformanceCounter();
    double ema_ms = 16.0;
    int auto_cooldown = 0;
    int calm_frames = 0;
    while (running) {
      SDL_Event event{};
      while (SDL_PollEvent(&event) != 0) {
        ImGui_ImplSDL2_ProcessEvent(&event);
        if (event.type == SDL_QUIT) {
          running = false;
        }
        if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
          running = false;
        }
        if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_F11) {
          render_prefs.fullscreen = !render_prefs.fullscreen;
        }
        if (!ImGui::GetIO().WantCaptureMouse) {
          if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
            dragging = true;
          }
          if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT) {
            dragging = false;
          }
          if (event.type == SDL_MOUSEMOTION && dragging) {
            orbit.orbit(-static_cast<float>(event.motion.xrel) * k_mouse_sensitivity,
                        -static_cast<float>(event.motion.yrel) * k_mouse_sensitivity);
          }
          if (event.type == SDL_MOUSEWHEEL) {
            orbit.zoom(event.wheel.y);
          }
          // Right-click drops a TDE star on the camera-facing side.
          if (event.type == SDL_MOUSEBUTTONDOWN &&
              event.button.button == SDL_BUTTON_RIGHT) {
            take_star_slot(stars).drop(comets, orbit.azimuth);
          }
        }
      }

      int drawable_width = 0;
      int drawable_height = 0;
      SDL_GL_GetDrawableSize(window.get(), &drawable_width, &drawable_height);

      // Fullscreen toggle (F11 / checkbox / --fullscreen): borderless
      // desktop mode, no display-mode change. FBOs follow resizes alone.
      const Uint32 want_fs = render_prefs.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0;
      if ((SDL_GetWindowFlags(window.get()) & SDL_WINDOW_FULLSCREEN_DESKTOP) != want_fs) {
        SDL_SetWindowFullscreen(window.get(), want_fs ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
      }

      if (drawable_width > 0 && drawable_height > 0) {
        if (orbit.auto_orbit && !dragging) {
          orbit.tick_auto_orbit();
        }
        const Camera camera = orbit.to_camera();
        const float aspect = static_cast<float>(drawable_width) /
                             static_cast<float>(drawable_height);
        const glm::mat4 projection = glm::perspective(
            glm::radians(camera.vertical_fov_degrees), aspect,
            k_projection_near, k_projection_far);
        const glm::mat4 view = glm::lookAt(camera.position, camera.target,
                                           glm::vec3{0.0F, 0.0F, 1.0F});

        if (comets.running) {
          comets.advance();  // frame-by-frame advance at the chosen speed
          for (StarEvent& star : stars) {
            star.advance(comets);  // TDE stars fall on the same clock
          }
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        draw_settings_ui(orbit, disk, hud_mass_solar, comets, render_prefs,
                         hud_spin, stars);

        // Spec section 5 mass slider now drives the scene: heavier = deeper
        // rubber-sheet funnel (compressed M^0.25) and slower orbital clocks
        // (Kepler T ~ M, sqrt-compressed; 1 M_sun keeps today's speed).
        const float mass_clamped = std::clamp(hud_mass_solar, 0.1F, 100.0F);
        const float grid_mass = k_geometric_mass * std::pow(mass_clamped, 0.25F);
        const float time_speed = 1.0F / std::sqrt(mass_clamped);
        comets.time_scale = time_speed;

        // Raytrace into the currently bound framebuffer at the given size
        // (full window, or scaled scene FBO when offscreen). The grid is
        // always drawn separately at full resolution so it stays crisp.
        auto draw_raytrace_at = [&](int target_width, int target_height) {
          glEnable(GL_DEPTH_TEST);
          glDepthFunc(GL_LEQUAL);
          glDepthMask(GL_TRUE);
          glClearColor(0.01F, 0.01F, 0.03F, 1.0F);
          glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
          raytrace_pass.draw(camera, disk, comets, stars, target_width,
                             target_height,
                             // Scene clock in geometric units (4M per frame
                             // at 60 fps): the disk pattern corotates at the
                             // true Keplerian rate; the mass factor is the
                             // artistic clock already documented.
                             static_cast<float>(frame) * 4.0F * time_speed,
                             render_prefs.pixel_size,
                             std::clamp(hud_spin, -0.95F, 0.95F),
                             glm::vec4(render_prefs.realism_plunge ? 1.0F
                                                                   : 0.0F,
                                       render_prefs.realism_jets ? 1.0F : 0.0F,
                                       render_prefs.realism_ergo ? 1.0F : 0.0F,
                                       render_prefs.realism_turbulence ? 1.0F
                                                                       : 0.0F));
        };

        // Crisp full-res grid, depth-tested against the scene depth (blitted
        // from the scaled FBO in the offscreen path, native in direct mode).
        // Occlusion at the disk silhouette follows the blitted depth blocks:
        // exact at full scale, chunky-consistent at low render scales.
        auto draw_grid_at = [&](int target_width, int target_height) {
          glViewport(0, 0, target_width, target_height);
          glEnable(GL_DEPTH_TEST);
          glDepthFunc(GL_LEQUAL);
          glDepthMask(GL_TRUE);
          grid_pass.draw(projection * view, target_width, target_height,
                         render_prefs.pixel_size, grid_mass);
        };

        if (render_prefs.polish || render_prefs.render_scale < 0.999F) {
          // Offscreen scene (polish chain and/or render scale): raytrace
          // renders small, then color (+depth) comes back up to the window
          // and the grid draws crisp at full resolution on top of it.
          const int scene_width =
              std::max(1, static_cast<int>(drawable_width *
                                           render_prefs.render_scale));
          const int scene_height =
              std::max(1, static_cast<int>(drawable_height *
                                           render_prefs.render_scale));
          glBindFramebuffer(GL_FRAMEBUFFER,
                            post_chain.scene_fbo(scene_width, scene_height));
          draw_raytrace_at(scene_width, scene_height);
          if (render_prefs.polish) {
            post_chain.render(scene_width, scene_height, drawable_width,
                              drawable_height, render_prefs.pixel_size,
                              render_prefs.bloom_strength);
            // Composite writes color only: bring the scene depth along so
            // the grid below depth-tests against the disk/horizon.
            post_chain.blit_depth_to_screen(drawable_width, drawable_height);
          } else {
            post_chain.blit_to_screen(drawable_width, drawable_height,
                                      render_prefs.upscale_nearest);
          }
          if (render_prefs.show_grid) {
            draw_grid_at(drawable_width, drawable_height);
          }
        } else {
          glBindFramebuffer(GL_FRAMEBUFFER, 0);
          draw_raytrace_at(drawable_width, drawable_height);
          if (render_prefs.show_grid) {
            draw_grid_at(drawable_width, drawable_height);
          }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        ImGui::Render();
        glDisable(GL_DEPTH_TEST);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glEnable(GL_DEPTH_TEST);

        const bool is_last_frame =
            options.frame_limit >= 0 && frame + 1 >= options.frame_limit;
        if (!options.screenshot_path.empty() && is_last_frame) {
          save_screenshot(drawable_width, drawable_height, options.screenshot_path);
        }
      }

      SDL_GL_SwapWindow(window.get());

      // Auto quality scaler: walk the render-scale steps (work scales with
      // the square) to hold ~60 fps. Disabled for --frames runs so
      // verification screenshots stay deterministic.
      const Uint64 now_ticks = SDL_GetPerformanceCounter();
      const double frame_ms = 1000.0 *
                              static_cast<double>(now_ticks - last_ticks) /
                              static_cast<double>(tick_freq);
      last_ticks = now_ticks;
      ema_ms += 0.08 * (frame_ms - ema_ms);
      if (render_prefs.auto_quality && options.frame_limit < 0) {
        if (--auto_cooldown <= 0) {
          if (ema_ms > 19.0 &&
              render_prefs.auto_scale_idx < kAutoScaleCount - 1) {
            ++render_prefs.auto_scale_idx;  // too slow: render smaller
            auto_cooldown = 30;
            calm_frames = 0;
          } else if (ema_ms < 11.0 && render_prefs.auto_scale_idx > 0) {
            if (++calm_frames >= 4) {  // sustained headroom before stepping up
              --render_prefs.auto_scale_idx;
              calm_frames = 0;
              auto_cooldown = 30;
            }
          } else {
            calm_frames = 0;
          }
          render_prefs.render_scale = kAutoScales[render_prefs.auto_scale_idx];
        }
      }

      ++frame;
      if (options.frame_limit >= 0 && frame >= options.frame_limit) {
        running = false;
      }
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
  } catch (const std::exception& error) {
    std::cerr << "fatal: " << error.what() << '\n';
    SDL_Quit();
    return EXIT_FAILURE;
  }

  SDL_Quit();
  return EXIT_SUCCESS;
}
