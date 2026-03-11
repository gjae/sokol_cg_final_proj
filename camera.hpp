#ifndef CAMERA_HPP
#define CAMERA_HPP

#include "HandmadeMath.h"

struct Camera {
  HMM_Vec3 position;
  float yaw;
  float pitch;

  HMM_Vec3 forward;
  HMM_Vec3 right;
  HMM_Vec3 up;

  Camera() {
    position = HMM_V3(0.0f, 1.0f, 0.0f); // Default height
    yaw = 0.0f;                          // Looking along +X axis
    pitch = 0.0f;
    update_vectors();
  }

  void update_vectors() {
    HMM_Vec3 front;
    front.X = HMM_CosF(HMM_AngleDeg(yaw)) * HMM_CosF(HMM_AngleDeg(pitch));
    front.Y = HMM_SinF(HMM_AngleDeg(pitch));
    front.Z = HMM_SinF(HMM_AngleDeg(yaw)) * HMM_CosF(HMM_AngleDeg(pitch));
    forward = HMM_NormV3(front);

    HMM_Vec3 world_up = HMM_V3(0.0f, 1.0f, 0.0f);
    right = HMM_NormV3(HMM_Cross(forward, world_up));
    up = HMM_NormV3(HMM_Cross(right, forward));
  }

  HMM_Mat4 get_view_matrix() {
    return HMM_LookAt_RH(position, HMM_AddV3(position, forward), up);
  }
};

#endif // CAMERA_HPP
