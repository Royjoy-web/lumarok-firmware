#pragma once
// ServoHAL is defined in RelayHAL.h alongside the relay and RGB HALs.
// This header exists so ActuatorTask.h can include it explicitly without
// relying on transitive inclusion through RelayHAL.h.
#include "RelayHAL.h"
