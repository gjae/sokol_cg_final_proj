#ifndef COLLISION_VERIFY_HPP
#define COLLISION_VERIFY_HPP

#include "HandmadeMath.h"
#include <algorithm>
#include <cmath>

struct Circle {
  HMM_Vec2 center;
  float radius;
};

struct AABB {
  HMM_Vec2 min;
  HMM_Vec2 max;
};

// Verifica colisión entre un Círculo y un AABB en 2D.
// Retorna true si hay colisión, y calcula el 'out_mtv' (Minimum Translation
// Vector) necesario para desplazar el círculo fuera del AABB.
inline bool check_circle_aabb_collision(const Circle &circle, const AABB &aabb,
                                        HMM_Vec2 *out_mtv) {
  // 1. Encontrar el punto más cercano en el AABB al centro del círculo
  float closest_x = std::max(aabb.min.X, std::min(circle.center.X, aabb.max.X));
  float closest_y = std::max(aabb.min.Y, std::min(circle.center.Y, aabb.max.Y));
  HMM_Vec2 closest = HMM_V2(closest_x, closest_y);

  // 2. Calcular la distancia entre el centro del círculo y este punto más
  // cercano
  HMM_Vec2 diff = HMM_SubV2(circle.center, closest);
  float distance_sq = HMM_DotV2(diff, diff);

  // 3. Verificar si la distancia es menor o igual al radio
  if (distance_sq <= circle.radius * circle.radius) {
    if (out_mtv) {
      float distance = std::sqrt(distance_sq);
      if (distance > 0.0001f) {
        // El centro está fuera del AABB (típico)
        float penetration = circle.radius - distance;
        HMM_Vec2 normal = HMM_MulV2F(diff, 1.0f / distance);
        *out_mtv = HMM_MulV2F(normal, penetration);
      } else {
        // El centro del círculo está exactamente dentro del AABB (distancia muy
        // pequeña o 0). Calculamos el MTV basado en el lado del AABB más
        // cercano.
        float p_right = aabb.max.X - circle.center.X;
        float p_left = circle.center.X - aabb.min.X;
        float p_top = aabb.max.Y - circle.center.Y;
        float p_bottom = circle.center.Y - aabb.min.Y;

        float min_p = std::min({p_right, p_left, p_top, p_bottom});

        if (min_p == p_right)
          *out_mtv = HMM_V2(p_right + circle.radius, 0.0f);
        else if (min_p == p_left)
          *out_mtv = HMM_V2(-(p_left + circle.radius), 0.0f);
        else if (min_p == p_top)
          *out_mtv = HMM_V2(0.0f, p_top + circle.radius);
        else
          *out_mtv = HMM_V2(0.0f, -(p_bottom + circle.radius));
      }
    }
    return true;
  }

  return false;
}

#endif // COLLISION_VERIFY_HPP
