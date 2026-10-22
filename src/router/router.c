#include "router.h"

#include <string.h>

static uint32_t method_flag(const char *method) {
  if (strcmp(method, "GET") == 0)
    return HTTP_ROUTE_GET;
  if (strcmp(method, "HEAD") == 0)
    return HTTP_ROUTE_HEAD;
  if (strcmp(method, "POST") == 0)
    return HTTP_ROUTE_POST;
  if (strcmp(method, "PUT") == 0)
    return HTTP_ROUTE_PUT;
  if (strcmp(method, "DELETE") == 0)
    return HTTP_ROUTE_DELETE;
  return 0;
}

void http_router_init(HttpRouter *router) {
  if (router != NULL)
    memset(router, 0, sizeof(*router));
}

int http_router_add(HttpRouter *router, const char *path,
                    HttpRoutePattern pattern, uint32_t method_mask,
                    HttpRouteAction action) {
  if (router == NULL || path == NULL || path[0] != '/' || method_mask == 0 ||
      strlen(path) >= HTTP_ROUTER_PATH_CAPACITY ||
      router->route_count >= HTTP_ROUTER_MAX_ROUTES ||
      (pattern != HTTP_ROUTE_EXACT && pattern != HTTP_ROUTE_PREFIX &&
       pattern != HTTP_ROUTE_FALLBACK) ||
      (action != HTTP_ROUTE_API && action != HTTP_ROUTE_STATIC) ||
      (pattern == HTTP_ROUTE_FALLBACK && strcmp(path, "/") != 0))
    return -1;

  for (size_t index = 0; index < router->route_count; index++) {
    const HttpRoute *existing = &router->routes[index];
    if (existing->pattern == pattern && strcmp(existing->path, path) == 0)
      return -1;
  }

  HttpRoute *route = &router->routes[router->route_count++];
  size_t path_length = strlen(path);
  memcpy(route->path, path, path_length + 1);
  route->pattern = pattern;
  route->method_mask = method_mask;
  route->action = action;
  return 0;
}

static size_t target_path_length(const char *target) {
  const char *query = strchr(target, '?');
  return query != NULL ? (size_t)(query - target) : strlen(target);
}

static int route_matches(const HttpRoute *route, const char *target,
                         size_t target_length, size_t *specificity) {
  if (route->pattern == HTTP_ROUTE_FALLBACK) {
    *specificity = 0;
    return 1;
  }
  size_t path_length = strlen(route->path);
  if (route->pattern == HTTP_ROUTE_EXACT) {
    if (path_length != target_length ||
        memcmp(route->path, target, target_length) != 0)
      return 0;
    *specificity = path_length + HTTP_ROUTER_PATH_CAPACITY;
    return 1;
  }
  if (target_length < path_length ||
      memcmp(route->path, target, path_length) != 0)
    return 0;
  /* Prefix routes end at a path-component boundary. */
  if (path_length != 0 && route->path[path_length - 1] != '/' &&
      target_length != path_length && target[path_length] != '/')
    return 0;
  *specificity = path_length;
  return 1;
}

HttpRouteResult http_router_resolve(const HttpRouter *router,
                                    const char *method, const char *target,
                                    const HttpRoute **route) {
  if (route != NULL)
    *route = NULL;
  if (router == NULL || method == NULL || target == NULL || target[0] != '/')
    return HTTP_ROUTE_NO_MATCH;

  uint32_t method_bit = method_flag(method);
  size_t target_length = target_path_length(target);
  const HttpRoute *best_route = NULL;
  size_t best_specificity = 0;
  for (size_t index = 0; index < router->route_count; index++) {
    const HttpRoute *candidate = &router->routes[index];
    size_t specificity = 0;
    if (route_matches(candidate, target, target_length, &specificity) &&
        (best_route == NULL || specificity > best_specificity)) {
      best_route = candidate;
      best_specificity = specificity;
    }
  }

  if (best_route == NULL)
    return HTTP_ROUTE_NO_MATCH;
  if (route != NULL)
    *route = best_route;
  if (method_bit == 0 || (best_route->method_mask & method_bit) == 0)
    return HTTP_ROUTE_METHOD_NOT_ALLOWED;
  return HTTP_ROUTE_MATCHED;
}
