/*
 * mcp_server.h - MCP (Model Context Protocol) endpoint on the device.
 *
 * The MCP server used to run on the host as a stdio process (tools/scale_mcp.py,
 * removed when the endpoint moved here). The point of this module is the other
 * direction: the MCP protocol is now served by the firmware itself, so any MCP
 * client can talk to the scale directly over the network - one POST to /mcp
 * per call, no host in the loop.
 *
 * Transport, the streaming-HTTP form of the protocol:
 *
 *   POST /mcp     body: one JSON-RPC 2.0 message
 *   reply:        application/json, the response object
 *   notification: 202 Accepted, empty body
 *
 * Methods: initialize, tools/list, tools/call (tools: get_weight, tare,
 * set_unit) and ping - the same tool set the host-side server exposed.
 */
#ifndef MCP_SERVER_H
#define MCP_SERVER_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"

/* HTTP handler for POST /mcp. */
esp_err_t mcp_handler(httpd_req_t *req);

/* Processes one JSON-RPC message: parses it, runs the tool and writes the
 * response into out (NUL-terminated, capped at out_cap). Returns true when a
 * response should be sent, and false for notifications (no id). */
bool mcp_process(char *body, size_t len, char *out, size_t out_cap);

#endif /* MCP_SERVER_H */
