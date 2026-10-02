#pragma once
#include <Arduino.h>

/* ===========================================================================
 * KUTSU Sense — master-node LAN dashboard + ingest API
 * ---------------------------------------------------------------------------
 * Only the elected MASTER runs this (core synchronous WebServer, not async). It:
 *   - serves a self-contained SPA (index_html.h) at GET /
 *   - accepts records from slaves at POST /api/ingest  (and ingests its own)
 *   - registers/updates nodes at POST /api/nodes/register
 *   - exposes GET /api/nodes (latest per node) and GET /api/health
 *   - browsers poll /api/nodes every 2 s for live updates (no WebSocket)
 *
 * A per-node registry (id/mac/room/ip/lastSeen/last record) is kept in RAM so
 * the dashboard can show a building overview AND per-room drill-down. Each node
 * still owns its durable JSONL on its own SD (req 2.5) — the master is the live
 * aggregation view, not the system of record.
 * =========================================================================== */

void webMasterStart();          // start async server + WS + mDNS "master" TXT
void webMasterStop();           // demoted to slave: tear the server down

// Feed one record (this node's own, or a slave's push) into the registry and
// fan it out to WebSocket browsers. `fromIp` may be nullptr for local records.
void webIngestRecord(const String& recordJson, const char* fromIp);

bool webMasterRunning();
