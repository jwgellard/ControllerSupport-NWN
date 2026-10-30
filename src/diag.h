// Stage-2 spike diagnostics (docs/DESIGN.md, milestone M0): one-shot snapshots of
// camera, objects, GUI panels and controls, written to nwn_controller.log.
#pragma once

#include "sdl_api.h"

void DiagSnapshot(SDL_Window* window);
