#ifndef HTTP_ROUTER_H
#define HTTP_ROUTER_H

#include <stddef.h>
#include <stdint.h>

#define HTTP_ROUTER_MAX_ROUTES 16U
#define HTTP_ROUTER_PATH_CAPACITY 128U

typedef enum HttpRouteAction {
  HTTP_ROUTE_API,
  HTTP_ROUTE_STATIC
} HttpRouteAction;

typedef enum HttpRoutePattern {
  HTTP_ROUTE_EXACT,
  HTTP_ROUTE_PREFIX,
  HTTP_ROUTE_FALLBACK
} HttpRoutePattern;

typedef enum HttpRouteResult {
  HTTP_ROUTE_NO_MATCH,
  HTTP_ROUTE_MATCHED,
  HTTP_ROUTE_METHOD_NOT_ALLOWED
} HttpRouteResult;

enum {
  HTTP_ROUTE_GET = 1U << 0,
  HTTP_ROUTE_HEAD = 1U << 1,
  HTTP_ROUTE_POST = 1U << 2,
  HTTP_ROUTE_PUT = 1U << 3,
  HTTP_ROUTE_DELETE = 1U << 4
};
#define HTTP_ROUTE_ALL_METHODS UINT32_MAX

typedef struct HttpRoute {
  char path[HTTP_ROUTER_PATH_CAPACITY];
  HttpRoutePattern pattern;
  HttpRouteAction action;
  uint32_t method_mask;
} HttpRoute;

typedef struct HttpRouter {
  HttpRoute routes[HTTP_ROUTER_MAX_ROUTES];
  size_t route_count;
} HttpRouter;

void http_router_init(HttpRouter *router);
int http_router_add(HttpRouter *router, const char *path,
                    HttpRoutePattern pattern, uint32_t method_mask,
                    HttpRouteAction action);
/* Returns the most specific route; the route pointer belongs to router. */
HttpRouteResult http_router_resolve(const HttpRouter *router,
                                    const char *method, const char *target,
                                    const HttpRoute **route);

#endif
