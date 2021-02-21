/*! \file */ 
#ifndef _TILE_WORKER_H
#define _TILE_WORKER_H

#include "shared_data.h"

/// Start function for worker threads
int outputProc(uint threadId, class SharedData *sharedData, OSMStore *osmStore, int srcZ, int srcX, int srcY);

#endif //_TILE_WORKER_H
