# KMB ETA + Stop Info (VisionMaster)

This project uses the public KMB endpoints from `https://data.etabus.gov.hk`.

## Key endpoints

- **Route list (discover valid bound/service_type)**:
  - `GET /v1/transport/kmb/route/`

- **Route info**:
  - `GET /v1/transport/kmb/route/{route}/{direction}/{service_type}`
  - `direction` is typically `outbound` or `inbound`.

- **Route stops (map seq -> stop_id)**:
  - Commonly seen shapes (project tries multiple for compatibility):
    - `GET /v1/transport/kmb/route-stop/{route}/{direction}/{service_type}`
    - `GET /v1/transport/kmb/route-stop/{route}/{bound}/{service_type}`
    - `GET /v1/transport/kmb/route-stop/{route}/{service_type}` (then filter by `bound` in returned items if present)

- **Stop (station) metadata**:
  - `GET /v1/transport/kmb/stop/{stop_id}`
  - Fields used: `stop`, `name_en`, `name_tc`, `lat`, `long`

- **ETA APIs** (three options available):
  - **ETA API** (currently used, but returning empty responses):
    - `GET /v1/transport/kmb/eta/{route}/{stop_id}/{service_type}`
    - Returns ETAs for specific stop/route/service_type combination
    - This project filters results by `dir` (`O`/`I`) via `DEFAULT_BOUND`.
  - **Route ETA API** (RECOMMENDED - more reliable):
    - `GET /v1/transport/kmb/route-eta/{route}/{service_type}`
    - Returns ETAs for all stops on the route
    - Filter by `seq` field (matches route-stop API) to get specific stop
    - Filter by `dir` field for direction
    - More reliable because it doesn't require stop_id validation
  - **Stop ETA API** (alternative fallback):
    - `GET /v1/transport/kmb/stop-eta/{stop_id}`
    - Returns ETAs for all routes serving that stop
    - Filter by `route` field in response to get specific route

## Common "HTTP 200 but 0 ETAs" causes

- `data: []`: valid request but **no matching ETA records** (often mismatched `stop_id` vs `route/service_type`, or no upcoming buses).
- `data` non-empty but **0 after filtering**: ETAs exist, but `dir` doesn't match `DEFAULT_BOUND`.

## Current Issue and Recommendation

**Problem:** ETA API returns empty `data: []` even with valid parameters.

**Root Cause:** Stop ID from route-stop API may not be directly usable in ETA API due to validation differences.

**Recommended Solution:** Use Route ETA API instead:
- Endpoint: `/v1/transport/kmb/route-eta/{route}/{service_type}`
- Filter results by `seq` field (already available from route-stop API)
- More reliable, fewer parameters, avoids stop_id validation issues

**See detailed analysis:** `knowledge_map/eta_api_analysis.md`


