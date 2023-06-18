/*! \file */ 
#ifndef _TILE_WORKER_H
#define _TILE_WORKER_H

#include "shared_data.h"
#include <boost/asio/thread_pool.hpp>

/// Start function for worker threads
bool outputProc(boost::asio::thread_pool &pool, SharedData &sharedData,
                SourceList const &sources, AttributeStore const &attributeStore,
                std::vector<std::vector<OutputObjectRef>> const &data, TileCoordinates coordinates, uint zoom);

#endif //_TILE_WORKER_H
