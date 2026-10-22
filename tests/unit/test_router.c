#include "router.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  HttpRouter router;
  http_router_init(&router);
  assert(http_router_add(&router, "/api", HTTP_ROUTE_EXACT,
                        HTTP_ROUTE_ALL_METHODS, HTTP_ROUTE_API) == 0);
  assert(http_router_add(&router, "/api/", HTTP_ROUTE_PREFIX,
                        HTTP_ROUTE_ALL_METHODS, HTTP_ROUTE_API) == 0);
  assert(http_router_add(&router, "/", HTTP_ROUTE_FALLBACK, HTTP_ROUTE_GET,
                        HTTP_ROUTE_STATIC) == 0);
  const HttpRoute *route = NULL;
  assert(http_router_resolve(&router, "GET", "/api?x=1", &route) ==
         HTTP_ROUTE_MATCHED);
  assert(route != NULL && route->action == HTTP_ROUTE_API);
  assert(http_router_resolve(&router, "POST", "/api/users/", &route) ==
         HTTP_ROUTE_MATCHED);
  assert(route != NULL && route->action == HTTP_ROUTE_API);
  assert(http_router_resolve(&router, "GET", "/apix", &route) ==
         HTTP_ROUTE_MATCHED);
  assert(route != NULL && route->action == HTTP_ROUTE_STATIC);
  assert(http_router_resolve(&router, "POST", "/about.html", &route) ==
         HTTP_ROUTE_METHOD_NOT_ALLOWED);
  assert(route != NULL && route->action == HTTP_ROUTE_STATIC);

  puts("Router unit tests passed");
  return 0;
}
