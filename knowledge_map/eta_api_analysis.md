# KMB ETA API Analysis and Comparison

## Overview

This document analyzes the three ETA-related APIs available in the KMB API specification (v1.05, 23 Oct 2024) and compares them with the current implementation.

## API Endpoints Summary

### 1. ETA API (Section 8)

**Endpoint:** `/v1/transport/kmb/eta/{route}/{stop}/{service_type}`

**HTTP Method:** GET

**Parameters:**
- `route` (Path, Required): The route number (case-sensitive)
- `stop` (Path, Required): The stop ID
- `service_type` (Path, Required): The service type of the bus route

**Response Structure:**
```json
{
  "type": "ETA",
  "version": "1.0",
  "generated_timestamp": "2021-03-18T13:39:50+08:00",
  "data": [
    {
      "co": "KMB",
      "route": "3M",
      "dir": "O",
      "service_type": 1,
      "seq": 1,
      "dest_tc": "彩雲",
      "dest_sc": "彩云",
      "dest_en": "CHOI WAN",
      "eta_seq": 1,
      "eta": "2021-03-18T13:50:00+08:00",
      "rmk_tc": "原定班次",
      "rmk_sc": "原定班次",
      "rmk_en": "Scheduled Bus",
      "data_timestamp": "2021-03-18T13:39:46+08:00"
    }
  ]
}
```

**Key Fields:**
- `dir`: Direction ("O" for outbound, "I" for inbound)
- `seq`: Stop sequence number on the route
- `eta`: ISO 8601 timestamp of estimated arrival
- `eta_seq`: Sequence of this ETA (1st, 2nd, 3rd bus, etc.)
- `dest_en/tc/sc`: Destination in English/Traditional Chinese/Simplified Chinese
- `rmk_en/tc/sc`: Remarks (e.g., "Scheduled Bus", "Delayed", etc.)

**Current Implementation:**
- Uses this API at line 810 in `src/main.cpp`
- URL format: `/v1/transport/kmb/eta/{route}/{stop}/{service_type}`
- Matches documented format ✓

**Issue Identified:**
- Returns empty `data: []` array
- Possible causes:
  1. Stop ID may not be valid for the route/service_type combination
  2. No buses scheduled at that time
  3. Service type mismatch

---

### 2. STOP ETA API (Section 9)

**Endpoint:** `/v1/transport/kmb/stop-eta/{stop_id}`

**HTTP Method:** GET

**Parameters:**
- `stop_id` (Path, Required): The stop ID (hexadecimal string)

**Response Structure:**
Returns ETAs for ALL routes serving that stop.

**Sample Response (inferred from API pattern):**
```json
{
  "type": "StopETA",
  "version": "1.0",
  "generated_timestamp": "2021-03-18T13:39:50+08:00",
  "data": [
    {
      "co": "KMB",
      "route": "299X",
      "dir": "O",
      "service_type": 1,
      "seq": 2,
      "dest_tc": "...",
      "dest_sc": "...",
      "dest_en": "...",
      "eta_seq": 1,
      "eta": "2021-03-18T13:50:00+08:00",
      "rmk_tc": "...",
      "rmk_sc": "...",
      "rmk_en": "...",
      "data_timestamp": "2021-03-18T13:39:46+08:00"
    },
    {
      "co": "KMB",
      "route": "299X",
      "dir": "I",
      "service_type": 1,
      "seq": 15,
      ...
    },
    {
      "co": "KMB",
      "route": "74B",
      "dir": "O",
      "service_type": 1,
      "seq": 5,
      ...
    }
  ]
}
```

**Key Features:**
- Returns ETAs for **ALL routes** that serve the stop
- Includes `route` field to identify which route each ETA belongs to
- Includes `dir` field (direction)
- Includes `seq` field (stop sequence on that route)
- Must filter by `route` in code to get ETAs for specific route
- Useful when you know the stop but want to see all routes

**Use Cases:**
1. Display all routes serving a stop
2. Fallback when route-specific APIs fail
3. Multi-route stop information

**Current Implementation:**
- NOT currently used
- Could be useful as a fallback or alternative approach
- Would require filtering by route in response data

**Advantages:**
- Simple parameter (just stop_id)
- No need to know route or service_type
- Returns comprehensive data

**Disadvantages:**
- Returns data for all routes (more data to process)
- Must filter by route in code
- Less efficient if you only need one route

---

### 3. ROUTE ETA API (Section 10)

**Endpoint:** `/v1/transport/kmb/route-eta/{route}/{service_type}`

**HTTP Method:** GET

**Parameters:**
- `route` (Path, Required): The route number (case-sensitive)
- `service_type` (Path, Required): The service type

**Response Structure:**
```json
{
  "type": "RouteETA",
  "version": "1.0",
  "generated_timestamp": "2021-03-18T13:39:50+08:00",
  "data": [
    {
      "co": "KMB",
      "route": "3M",
      "dir": "O",
      "service_type": 1,
      "seq": 1,
      "dest_tc": "彩雲",
      "dest_sc": "彩云",
      "dest_en": "CHOI WAN",
      "eta_seq": 1,
      "eta": "2021-03-18T13:50:00+08:00",
      "rmk_tc": "原定班次",
      "rmk_sc": "原定班次",
      "rmk_en": "Scheduled Bus",
      "data_timestamp": "2021-03-18T13:39:46+08:00"
    },
    {
      "co": "KMB",
      "route": "3M",
      "dir": "O",
      "service_type": 1,
      "seq": 1,
      "dest_tc": "彩雲",
      "dest_sc": "彩云",
      "dest_en": "CHOI WAN",
      "eta_seq": 2,
      "eta": "2021-03-18T14:10:00+08:00",
      "rmk_tc": "原定班次",
      "rmk_sc": "原定班次",
      "rmk_en": "Scheduled Bus",
      "data_timestamp": "2021-03-18T13:39:46+08:00"
    },
    {
      "co": "KMB",
      "route": "3M",
      "dir": "O",
      "service_type": 1,
      "seq": 2,
      "dest_tc": "彩雲",
      "dest_sc": "彩云",
      "dest_en": "CHOI WAN",
      "eta_seq": 1,
      "eta": "2021-03-18T13:51:29+08:00",
      "rmk_tc": "原定班次",
      "rmk_sc": "原定班次",
      "rmk_en": "Scheduled Bus",
      "data_timestamp": "2021-03-18T13:39:46+08:00"
    }
  ]
}
```

**Key Features:**
- Returns ETAs for **ALL stops** on the route
- Includes `seq` field to identify which stop each ETA belongs to
- Multiple `eta_seq` entries per stop (1st bus, 2nd bus, 3rd bus, etc.)
- Can filter results by `seq` to get ETAs for a specific stop
- More reliable because it doesn't require matching stop_id to route/service_type

**Current Implementation:**
- NOT currently used
- **RECOMMENDED** as primary or fallback method

---

## Comparison Table

| Feature | ETA API | STOP ETA API | ROUTE ETA API |
|---------|---------|--------------|--------------|
| **Endpoint** | `/eta/{route}/{stop}/{service_type}` | `/stop-eta/{stop_id}` | `/route-eta/{route}/{service_type}` |
| **Parameters** | route, stop, service_type | stop_id only | route, service_type |
| **Returns** | ETAs for one stop | ETAs for all routes at one stop | ETAs for all stops on route |
| **Stop Filtering** | By stop_id in URL | N/A (single stop) | By `seq` field in response |
| **Route Filtering** | By route in URL | In response data | By route in URL |
| **Reliability** | ⚠️ Requires exact stop_id match | ✅ Simple, single parameter | ✅✅ Most reliable, includes seq |
| **Use Case** | Specific stop on specific route | All routes at a stop | All stops on a route |

---

## Current Implementation Analysis

### Code Location
- **File:** `src/main.cpp`
- **Function:** `fetchETAInfo()` (lines 800-1058)
- **Current API Used:** ETA API (`/v1/transport/kmb/eta/{route}/{stop}/{service_type}`)

### Current Flow
1. Gets route info to determine service_type
2. Lists route stops to get stop_id by sequence
3. Fetches stop info for display name
4. Calls ETA API with route, stop_id, service_type
5. **Problem:** Returns empty `data: []`

### Issue Analysis

**From the logs:**
- Route: 299X
- Stop ID: EB0D8F8D1AF9DDDE (seq 2, bound O, service_type 1)
- URL: `https://data.etabus.gov.hk/v1/transport/kmb/eta/299X/EB0D8F8D1AF9DDDE/1`
- Response: `{"type":"ETA","version":"1.0","generated_timestamp":"2026-01-12T08:55:21+08:00","data":[]}`

**Possible Root Causes:**

1. **Stop ID Validation Issue:**
   - The stop_id `EB0D8F8D1AF9DDDE` was obtained from route-stop API
   - However, the ETA API may require the stop_id to be valid for the specific route/service_type combination
   - The route-stop API may return stop_ids that are valid for the route but not for ETA queries

2. **Service Type Mismatch:**
   - Service type 1 might not be correct for this stop
   - Route 299X might have multiple service types, and seq 2 might be served by a different service_type

3. **Timing Issue:**
   - No buses scheduled at that time (08:55 on 2026-01-12)
   - However, this seems unlikely as the API should still return scheduled times

4. **API Parameter Format:**
   - Case sensitivity: Route "299X" must match exactly
   - Stop ID format: May need to be validated

---

## Recommendations

### 1. Implement Route ETA API as Primary Method (HIGH PRIORITY)

**Why:**
- Most reliable - returns ETAs for all stops on the route
- Includes `seq` field, which we already have from route-stop API
- Avoids stop_id validation issues
- Single API call gets all stop ETAs

**Implementation:**
```cpp
// New function: fetchRouteETAInfo()
// Endpoint: /v1/transport/kmb/route-eta/{route}/{service_type}
// Filter results by seq field to get ETAs for our target stop
```

**Benefits:**
- No need to match stop_id to route/service_type
- Can get ETAs for multiple stops in one call
- More resilient to API changes

### 2. Use ETA API as Fallback (MEDIUM PRIORITY)

**Keep current implementation but:**
- Add validation to verify stop_id is from route-stop API
- Try multiple service_types automatically
- Add better error messages

### 3. Add STOP ETA API as Alternative Fallback (LOW PRIORITY)

**Use when:**
- Route ETA API fails
- ETA API fails
- Can filter by route in response data

### 4. Improve Diagnostics

**Add:**
- Log the exact stop_id being used
- Log the route-stop API response for the selected stop
- Compare stop_id format between route-stop and ETA APIs
- Try all service_types automatically and log results

### 5. Validation Checks

**Before calling ETA API:**
- Verify stop_id exists in route-stop list for the route/service_type
- Check if stop has `bound` field matching our target bound
- Validate service_type matches route info

---

## Implementation Priority

1. **IMMEDIATE:** Implement Route ETA API as primary method
2. **SHORT TERM:** Keep ETA API as fallback with improved error handling
3. **MEDIUM TERM:** Add STOP ETA API as additional fallback
4. **ONGOING:** Improve diagnostics and logging

---

## Response Structure Comparison

### Detailed Response Structure Analysis

#### ETA API Response Structure

**Endpoint:** `/v1/transport/kmb/eta/{route}/{stop}/{service_type}`

**Response Type:** `"type": "ETA"`

**Response Scope:**
- Single stop (specified in URL)
- Single route (specified in URL)
- Single service_type (specified in URL)
- Multiple buses (different `eta_seq` values)

**Response Fields:**
```json
{
  "type": "ETA",
  "version": "1.0",
  "generated_timestamp": "2021-03-18T13:39:50+08:00",
  "data": [
    {
      "co": "KMB",                    // Company
      "route": "299X",                 // Route number
      "dir": "O",                      // Direction: "O" or "I"
      "service_type": 1,               // Service type
      "seq": 2,                        // Stop sequence on route
      "dest_tc": "...",                // Destination (TC)
      "dest_sc": "...",                // Destination (SC)
      "dest_en": "...",                // Destination (EN)
      "eta_seq": 1,                    // ETA sequence (1st, 2nd, 3rd bus)
      "eta": "2021-03-18T13:50:00+08:00", // ISO 8601 timestamp
      "rmk_tc": "...",                 // Remark (TC)
      "rmk_sc": "...",                 // Remark (SC)
      "rmk_en": "Scheduled Bus",       // Remark (EN)
      "data_timestamp": "2021-03-18T13:39:46+08:00" // Data timestamp
    }
  ]
}
```

**Key Characteristics:**
- All entries have same `route`, `stop` (implied), `service_type`
- All entries have same `seq` (same stop)
- Different `eta_seq` values (1st bus, 2nd bus, 3rd bus, etc.)
- May have different `dir` if stop serves both directions
- Empty array `[]` if no ETAs available

**Current Issue:**
- Returns empty array even with valid parameters
- Suggests stop_id validation or service_type mismatch

---

#### STOP ETA API Response Structure

**Endpoint:** `/v1/transport/kmb/stop-eta/{stop_id}`

**Response Type:** `"type": "StopETA"` (inferred)

**Response Scope:**
- Single stop (specified in URL)
- Multiple routes (all routes serving that stop)
- Multiple service_types (if routes have different types)
- Multiple buses per route

**Response Fields (inferred):**
```json
{
  "type": "StopETA",
  "version": "1.0",
  "generated_timestamp": "2021-03-18T13:39:50+08:00",
  "data": [
    {
      "co": "KMB",
      "route": "299X",                 // Route number (varies)
      "dir": "O",                      // Direction (varies)
      "service_type": 1,               // Service type (varies)
      "seq": 2,                        // Stop sequence on THIS route
      "dest_tc": "...",
      "dest_sc": "...",
      "dest_en": "...",
      "eta_seq": 1,                    // ETA sequence
      "eta": "2021-03-18T13:50:00+08:00",
      "rmk_tc": "...",
      "rmk_sc": "...",
      "rmk_en": "Scheduled Bus",
      "data_timestamp": "2021-03-18T13:39:46+08:00"
    },
    {
      "co": "KMB",
      "route": "74B",                  // Different route
      "dir": "I",                      // Different direction
      "service_type": 1,
      "seq": 5,                        // Different seq (on route 74B)
      ...
    }
  ]
}
```

**Key Characteristics:**
- All entries have same `stop_id` (implied, specified in URL)
- Different `route` values (multiple routes serve the stop)
- Different `seq` values (stop position varies by route)
- Different `dir` values (stop may serve both directions)
- Must filter by `route` in code to get specific route ETAs
- Must filter by `dir` in code to get specific direction

**Filtering Required:**
```cpp
// Filter by route
if (etaData["route"].as<String>() != targetRoute) continue;

// Filter by direction
if (etaData["dir"].as<String>() != targetBound) continue;

// Filter by service_type (if needed)
if (etaData["service_type"].as<int>() != targetServiceType) continue;
```

---

#### ROUTE ETA API Response Structure

**Endpoint:** `/v1/transport/kmb/route-eta/{route}/{service_type}`

**Response Type:** `"type": "RouteETA"`

**Response Scope:**
- Single route (specified in URL)
- Single service_type (specified in URL)
- Multiple stops (all stops on the route)
- Multiple buses per stop

**Response Fields (from documentation):**
```json
{
  "type": "RouteETA",
  "version": "1.0",
  "generated_timestamp": "2021-03-18T13:39:50+08:00",
  "data": [
    {
      "co": "KMB",
      "route": "3M",                   // Route (same for all)
      "dir": "O",                      // Direction (may vary)
      "service_type": 1,               // Service type (same for all)
      "seq": 1,                        // Stop sequence (varies)
      "dest_tc": "彩雲",
      "dest_sc": "彩云",
      "dest_en": "CHOI WAN",
      "eta_seq": 1,                    // ETA sequence (varies)
      "eta": "2021-03-18T13:50:00+08:00",
      "rmk_tc": "原定班次",
      "rmk_sc": "原定班次",
      "rmk_en": "Scheduled Bus",
      "data_timestamp": "2021-03-18T13:39:46+08:00"
    },
    {
      "co": "KMB",
      "route": "3M",
      "dir": "O",
      "service_type": 1,
      "seq": 1,                        // Same stop (seq=1)
      "dest_tc": "彩雲",
      "dest_sc": "彩云",
      "dest_en": "CHOI WAN",
      "eta_seq": 2,                    // 2nd bus for seq=1
      "eta": "2021-03-18T14:10:00+08:00",
      ...
    },
    {
      "co": "KMB",
      "route": "3M",
      "dir": "O",
      "service_type": 1,
      "seq": 2,                        // Next stop (seq=2)
      "dest_tc": "彩雲",
      "dest_sc": "彩云",
      "dest_en": "CHOI WAN",
      "eta_seq": 1,                    // 1st bus for seq=2
      "eta": "2021-03-18T13:51:29+08:00",
      ...
    }
  ]
}
```

**Key Characteristics:**
- All entries have same `route` and `service_type` (specified in URL)
- Different `seq` values (different stops on the route)
- Multiple entries per `seq` (different `eta_seq` values = different buses)
- May have different `dir` if route serves both directions
- Must filter by `seq` in code to get specific stop ETAs
- Must filter by `dir` in code to get specific direction

**Filtering Required:**
```cpp
// Filter by sequence (we have this from route-stop API)
if (etaData["seq"].as<int>() != targetSeq) continue;

// Filter by direction
if (etaData["dir"].as<String>() != targetBound) continue;

// Sort by eta_seq to get 1st, 2nd, 3rd bus
// Take first MAX_ETAS entries
```

---

### Response Structure Comparison Table

| Feature | ETA API | STOP ETA API | ROUTE ETA API |
|---------|---------|--------------|---------------|
| **Response Type** | "ETA" | "StopETA" | "RouteETA" |
| **Route in Response** | Same (from URL) | Varies (multiple routes) | Same (from URL) |
| **Stop in Response** | Same (from URL) | Same (from URL) | Varies (multiple stops) |
| **Service Type** | Same (from URL) | Varies (multiple routes) | Same (from URL) |
| **Sequence (seq)** | Same (one stop) | Varies (by route) | Varies (by stop) |
| **Direction (dir)** | May vary | Varies (by route) | May vary |
| **ETA Sequence** | Varies (1, 2, 3...) | Varies (1, 2, 3...) | Varies (1, 2, 3...) |
| **Filtering Needed** | By `dir` (optional) | By `route`, `dir` | By `seq`, `dir` |
| **Data Volume** | Small (one stop) | Medium (all routes) | Large (all stops) |
| **Use Case** | Specific stop/route | All routes at stop | All stops on route |

### Response Field Details

**Common Fields (all three APIs):**
- `co`: Company ("KMB")
- `route`: Route number
- `dir`: Direction ("O" or "I")
- `service_type`: Service type (integer)
- `seq`: Stop sequence on route
- `dest_tc/sc/en`: Destination names
- `eta_seq`: ETA sequence (1st, 2nd, 3rd bus)
- `eta`: ISO 8601 timestamp
- `rmk_tc/sc/en`: Remarks
- `data_timestamp`: When data was generated

**Field Variations:**
- **ETA API**: `route`, `service_type`, `seq` are constant (from URL)
- **STOP ETA API**: `route`, `seq` vary (multiple routes)
- **ROUTE ETA API**: `seq` varies (multiple stops)

### Empty Response Handling

**All APIs return empty array when:**
- No buses scheduled
- Invalid parameters
- No data available

**Current Issue:**
- ETA API returns `{"type":"ETA","version":"1.0","generated_timestamp":"...","data":[]}`
- Valid JSON structure but empty data
- Suggests parameter mismatch or no scheduled buses

**Route ETA API Advantage:**
- Less likely to return empty (covers all stops)
- Even if target stop has no buses, can see other stops have buses
- Easier to diagnose issues

---

## Executive Summary

### Problem Statement

The current implementation uses the ETA API (`/v1/transport/kmb/eta/{route}/{stop}/{service_type}`) but consistently receives empty `data: []` responses, even when:
- Route exists and is valid (299X)
- Stop ID is obtained from route-stop API (EB0D8F8D1AF9DDDE)
- Service type matches route info (1)
- Stop info API successfully returns stop details

### Root Cause Analysis

**Primary Issue: Stop ID Validation Mismatch**
- Stop ID from route-stop API may not be directly usable in ETA API
- Different validation rules between APIs
- Stop ID format or context requirements differ

**Secondary Issues:**
- Service type may not match between route API and ETA API
- Timing issues (no buses scheduled)
- Case sensitivity or parameter format issues

### Recommended Solution

**Switch to Route ETA API as Primary Method**

**Why:**
1. Eliminates stop_id validation issues (uses `seq` instead)
2. More reliable (fewer parameters, less validation)
3. Better data consistency (`seq` matches route-stop API)
4. Fewer API calls (faster, more efficient)
5. Easier to debug (comprehensive response)

**Implementation:**
- Use `/v1/transport/kmb/route-eta/{route}/{service_type}`
- Filter results by `seq` field (already have from route-stop API)
- Filter by `dir` field for direction
- Sort by `eta_seq` for bus order

**Fallback Strategy:**
1. Primary: Route ETA API
2. Fallback 1: ETA API (current method)
3. Fallback 2: Stop ETA API
4. Fallback 3: Try different service_types

### Key Findings

1. **Three ETA APIs Available:**
   - ETA API: `/eta/{route}/{stop}/{service_type}` (currently used, failing)
   - STOP ETA API: `/stop-eta/{stop_id}` (not used, could be fallback)
   - ROUTE ETA API: `/route-eta/{route}/{service_type}` (recommended)

2. **Parameter Requirements:**
   - Route: Case-sensitive, format "299X", "3M", etc.
   - Stop ID: Hexadecimal string, validation differs between APIs
   - Service Type: Integer (1, 2, etc.), must match route
   - Direction: "O"/"I" or "outbound"/"inbound" depending on API
   - Sequence: Integer, matches between route-stop and route-eta APIs

3. **Response Structures:**
   - All APIs return similar JSON structure
   - All include `seq`, `dir`, `eta_seq`, `eta` fields
   - Route ETA API includes all stops (filter by `seq`)
   - STOP ETA API includes all routes (filter by `route`)
   - ETA API includes only one stop (no filtering needed, but failing)

4. **Current Implementation Issues:**
   - Stop ID from route-stop API doesn't work in ETA API
   - Empty responses suggest parameter mismatch
   - Multiple fallback attempts all fail

### Action Items

1. **IMMEDIATE:** Implement Route ETA API as primary method
2. **SHORT TERM:** Keep ETA API as fallback with improved error handling
3. **MEDIUM TERM:** Add STOP ETA API as additional fallback
4. **ONGOING:** Improve diagnostics and logging

### Documentation Updates

- Created: `knowledge_map/eta_api_analysis.md` (this document)
- Updated: `knowledge_map/kmb_eta.md` (add reference to analysis)
- Code: `src/main.cpp` (needs Route ETA API implementation)

---

## Appendix: API Endpoint Reference

### Quick Reference

| API | Endpoint | Parameters | Returns |
|-----|----------|------------|---------|
| **Route List** | `/v1/transport/kmb/route/` | None | All routes |
| **Route** | `/v1/transport/kmb/route/{route}/{direction}/{service_type}` | route, direction, service_type | Route info |
| **Route-Stop** | `/v1/transport/kmb/route-stop/{route}/{direction}/{service_type}` | route, direction, service_type | All stops on route |
| **Stop** | `/v1/transport/kmb/stop/{stop_id}` | stop_id | Stop details |
| **ETA** | `/v1/transport/kmb/eta/{route}/{stop}/{service_type}` | route, stop, service_type | ETAs for one stop |
| **Stop ETA** | `/v1/transport/kmb/stop-eta/{stop_id}` | stop_id | ETAs for all routes at stop |
| **Route ETA** | `/v1/transport/kmb/route-eta/{route}/{service_type}` | route, service_type | ETAs for all stops on route |

### Base URL
All APIs: `https://data.etabus.gov.hk`

### Important Notes
- All URLs and parameters are case-sensitive
- Route numbers: "299X", "3M", "74B" (case matters)
- Direction: "outbound"/"inbound" or "O"/"I" depending on API
- Service type: Integer (1, 2, etc.)
- Stop ID: Hexadecimal string (case may matter)

---

## Parameter Requirements Comparison

### Route Parameter

**All APIs require route number, but format may vary:**

| API | Route Parameter | Format | Case Sensitive | Example |
|-----|----------------|--------|----------------|---------|
| Route API | `{route}` | Route number | Yes | "299X", "3M", "74B" |
| Route-Stop API | `{route}` | Route number | Yes | "299X", "3M", "74B" |
| ETA API | `{route}` | Route number | Yes | "299X", "3M", "74B" |
| Route ETA API | `{route}` | Route number | Yes | "299X", "3M", "74B" |
| Stop ETA API | N/A | N/A | N/A | N/A |

**Current Implementation:**
- Uses `ROUTE_NUMBER = "299X"` constant
- Passed directly to API calls
- Format appears correct ✓

### Stop ID Parameter

**Critical difference between APIs:**

| API | Stop Parameter | Source | Format | Example |
|-----|---------------|--------|---------|----------|
| Route-Stop API | Returns `stop` field | Response data | Hexadecimal string | "EB0D8F8D1AF9DDDE" |
| ETA API | `{stop}` in URL | Must match route-stop | Hexadecimal string | "EB0D8D1AF9DDDE" |
| Stop ETA API | `{stop_id}` in URL | Any valid stop_id | Hexadecimal string | "EB0D8F8D1AF9DDDE" |
| Route ETA API | N/A (uses `seq` instead) | N/A | N/A | N/A |

**Current Implementation:**
- Gets stop_id from route-stop API: `item["stop"].as<String>()`
- Uses this stop_id directly in ETA API call
- **Potential Issue:** Stop ID from route-stop may not be valid for ETA API

**Stop ID Format:**
- Hexadecimal string, typically 16 characters
- Case may matter (needs verification)
- Must be valid for the specific route/service_type combination

### Service Type Parameter

**All route-related APIs require service_type:**

| API | Service Type Parameter | Type | Valid Values | Example |
|-----|------------------------|------|--------------|---------|
| Route API | `{service_type}` | Integer | 1, 2, etc. | 1 |
| Route-Stop API | `{service_type}` | Integer | 1, 2, etc. | 1 |
| ETA API | `{service_type}` | Integer | 1, 2, etc. | 1 |
| Route ETA API | `{service_type}` | Integer | 1, 2, etc. | 1 |
| Stop ETA API | N/A | N/A | N/A | N/A |

**Current Implementation:**
- Gets service_type from route API response: `routeData["service_type"].as<int>()`
- Falls back to `DEFAULT_SERVICE_TYPE = 1`
- Tries multiple service_types (1, 2) as fallback
- **Potential Issue:** Service type from route API may not match what ETA API expects

### Direction/Bound Parameter

**Different APIs use different formats:**

| API | Direction Parameter | Format | Values | Conversion |
|-----|---------------------|--------|--------|------------|
| Route API | `{direction}` | String | "outbound", "inbound" | "O" → "outbound", "I" → "inbound" |
| Route-Stop API | `{direction}` or `{bound}` | String | "outbound"/"inbound" or "O"/"I" | Tries both formats |
| ETA API | N/A (in response `dir` field) | String | "O", "I" | Filtered in code |
| Route ETA API | N/A (in response `dir` field) | String | "O", "I" | Filtered in code |
| Stop ETA API | N/A (in response `dir` field) | String | "O", "I" | Filtered in code |

**Current Implementation:**
- Uses `convertDirectionToAPI()` to convert "O"/"I" to "outbound"/"inbound"
- For Route API: converts to "outbound"/"inbound" ✓
- For Route-Stop API: tries both formats ✓
- For ETA API: filters by `dir` field in response (currently disabled for diagnostics)

**Direction Conversion:**
```cpp
String convertDirectionToAPI(const char* bound) {
    if (strcmp(bound, "I") == 0 || strcmp(bound, "i") == 0) {
        return "inbound";
    } else if (strcmp(bound, "O") == 0 || strcmp(bound, "o") == 0) {
        return "outbound";
    }
    return String(bound);
}
```

### Sequence Parameter

**Only Route-Stop and Route ETA APIs use sequence:**

| API | Sequence Parameter | Usage | Source |
|-----|-------------------|-------|--------|
| Route-Stop API | `seq` in response | Identifies stop position | Response data |
| Route ETA API | `seq` in response | Identifies which stop ETA belongs to | Response data |
| ETA API | N/A | N/A | N/A |
| Stop ETA API | N/A | N/A | N/A |

**Current Implementation:**
- Gets `seq` from route-stop API: `item["seq"].as<int>()`
- Uses `seq` to select stop: `selectedStopSeq = DEFAULT_STOP_SEQ` (2)
- **Key Insight:** Route ETA API also includes `seq`, so we can filter by it!

---

## Parameter Validation Issues

### Issue 1: Stop ID Mismatch

**Problem:**
- Route-Stop API returns stop_id: `EB0D8F8D1AF9DDDE`
- This stop_id is used in ETA API call
- ETA API returns empty array

**Possible Causes:**
1. Stop ID format mismatch (case sensitivity?)
2. Stop ID not valid for route/service_type combination in ETA API
3. Stop ID valid for route-stop but not for ETA queries

**Evidence from Logs:**
- Route-Stop API successfully returns stop_id for seq 2
- Stop Info API successfully returns stop details for that stop_id
- ETA API returns empty array for same stop_id

**Solution:**
- Use Route ETA API instead (doesn't require stop_id)
- Filter by `seq` field which we already have

### Issue 2: Service Type Mismatch

**Problem:**
- Route API returns service_type: 1
- Route-Stop API called with service_type: 1
- ETA API called with service_type: 1
- But ETA API returns empty array

**Possible Causes:**
1. Service type 1 may not serve this specific stop
2. Different service types may serve different stops on the same route
3. Service type from route API may not match what ETA API expects

**Current Mitigation:**
- Code already tries service_type 1, then DEFAULT_SERVICE_TYPE, then 2
- All return empty arrays

**Solution:**
- Route ETA API returns all service types in one call (if multiple exist)
- Can filter by service_type in response data

### Issue 3: Case Sensitivity

**Documentation states:** "Please note that URL and parameters in API requests are case-sensitive."

**Current Implementation:**
- Route: "299X" (uppercase X) ✓
- Direction: Converted to lowercase "outbound"/"inbound" ✓
- Stop ID: Used as-is from API response (typically uppercase hex) ✓
- Service Type: Integer, no case issue ✓

**Verification Needed:**
- Stop ID case sensitivity (hex strings are typically case-insensitive, but API may require specific case)

---

## Conclusion

The **Route ETA API** is the most suitable for this use case because:
1. We already have the `seq` number from route-stop API
2. It avoids stop_id validation issues
3. It's more reliable (single parameter: route + service_type)
4. It returns comprehensive data that can be filtered by `seq`
5. Avoids parameter format mismatches between route-stop and ETA APIs

The current ETA API approach is failing likely due to stop_id validation or service_type mismatch issues. Switching to Route ETA API should resolve the empty response problem.

**Key Finding:** The stop_id obtained from route-stop API may not be directly usable in ETA API, even though it's valid for the route. This suggests the APIs may have different validation rules or the stop_id needs additional context (like bound/direction) that isn't being passed correctly.

---

## Route ETA API: Detailed Analysis

### Why Route ETA API is Better

**1. Eliminates Stop ID Validation Issues**
- Current problem: Stop ID from route-stop API doesn't work in ETA API
- Route ETA API solution: No stop_id needed, uses `seq` instead
- We already have `seq` from route-stop API (seq=2 in current case)

**2. Single Source of Truth**
- Route ETA API: One call gets ETAs for all stops
- Current approach: Multiple calls (route → route-stop → stop → eta)
- Fewer API calls = faster, more reliable

**3. Better Data Consistency**
- Route ETA API returns `seq` field matching route-stop API
- Can directly match: route-stop seq=2 → route-eta seq=2
- No need to validate stop_id compatibility

**4. Handles Multiple Service Types**
- Route ETA API returns data for the specified service_type
- If route has multiple service types, can call multiple times
- Response includes service_type in each entry for verification

**5. More Resilient**
- Doesn't depend on stop_id format or validation
- Works as long as route and service_type are correct
- Less prone to API changes affecting stop_id format

### Route ETA API Response Structure

From documentation, Route ETA API returns:
```json
{
  "type": "RouteETA",
  "version": "1.0",
  "generated_timestamp": "2021-03-18T13:39:50+08:00",
  "data": [
    {
      "co": "KMB",
      "route": "3M",
      "dir": "O",              // Direction: "O" or "I"
      "service_type": 1,        // Service type
      "seq": 1,                 // Stop sequence (matches route-stop API)
      "dest_tc": "彩雲",
      "dest_sc": "彩云",
      "dest_en": "CHOI WAN",
      "eta_seq": 1,             // ETA sequence (1st bus, 2nd bus, etc.)
      "eta": "2021-03-18T13:50:00+08:00",
      "rmk_tc": "原定班次",
      "rmk_sc": "原定班次",
      "rmk_en": "Scheduled Bus",
      "data_timestamp": "2021-03-18T13:39:46+08:00"
    },
    {
      "co": "KMB",
      "route": "3M",
      "dir": "O",
      "service_type": 1,
      "seq": 1,                 // Same stop (seq=1)
      "dest_tc": "彩雲",
      "dest_sc": "彩云",
      "dest_en": "CHOI WAN",
      "eta_seq": 2,             // 2nd bus for this stop
      "eta": "2021-03-18T14:10:00+08:00",
      ...
    },
    {
      "co": "KMB",
      "route": "3M",
      "dir": "O",
      "service_type": 1,
      "seq": 2,                 // Next stop (seq=2)
      "dest_tc": "彩雲",
      "dest_sc": "彩云",
      "dest_en": "CHOI WAN",
      "eta_seq": 1,             // 1st bus for seq=2 stop
      "eta": "2021-03-18T13:51:29+08:00",
      ...
    }
  ]
}
```

**Key Observations:**
- Multiple entries per stop (different `eta_seq` values)
- Multiple stops in one response (different `seq` values)
- Can filter by `seq` to get ETAs for specific stop
- Can filter by `dir` to get only outbound/inbound
- Can filter by `eta_seq` to get 1st, 2nd, 3rd bus, etc.

### Implementation Strategy

**Step 1: Call Route ETA API**
```
GET /v1/transport/kmb/route-eta/{route}/{service_type}
Example: /v1/transport/kmb/route-eta/299X/1
```

**Step 2: Filter Results**
- Filter by `seq` == target sequence (e.g., seq=2)
- Filter by `dir` == target direction (e.g., "O" for outbound)
- Sort by `eta_seq` to get 1st, 2nd, 3rd bus
- Take first MAX_ETAS entries

**Step 3: Fallback Logic**
- If Route ETA API fails, try ETA API as fallback
- If service_type doesn't work, try other service_types
- If route doesn't work, verify route exists

### Code Structure Recommendation

```cpp
// New function: fetchRouteETAInfo()
bool fetchRouteETAInfo(const char* route, int service_type, int targetSeq, const char* targetBound) {
    // 1. Call Route ETA API
    String url = String(API_BASE_URL) + "/v1/transport/kmb/route-eta/" + 
                 String(route) + "/" + String(service_type);
    
    // 2. Parse response
    // 3. Filter by seq == targetSeq
    // 4. Filter by dir == targetBound
    // 5. Sort by eta_seq
    // 6. Take first MAX_ETAS entries
    // 7. Populate etaList[]
}
```

### Advantages Over Current Approach

| Aspect | Current (ETA API) | Proposed (Route ETA API) |
|--------|------------------|--------------------------|
| **Parameters** | route, stop_id, service_type | route, service_type |
| **Stop ID Required** | Yes (problematic) | No (uses seq instead) |
| **API Calls** | 4 calls (route → route-stop → stop → eta) | 2 calls (route → route-eta) |
| **Reliability** | Fails with empty array | More reliable |
| **Data Consistency** | Stop ID may not match | Seq always matches |
| **Error Handling** | Hard to diagnose | Easier to debug |
| **Performance** | Slower (multiple calls) | Faster (fewer calls) |

### Migration Path

**Phase 1: Add Route ETA API as Primary**
- Implement `fetchRouteETAInfo()` function
- Use it as primary method in `setup()` and `loop()`
- Keep current ETA API as fallback

**Phase 2: Improve Fallback Logic**
- If Route ETA API fails, try ETA API
- If service_type fails, try other service_types
- Add comprehensive error logging

**Phase 3: Remove Old Code (Optional)**
- Once Route ETA API is proven reliable
- Can remove ETA API code if desired
- Or keep as additional fallback

