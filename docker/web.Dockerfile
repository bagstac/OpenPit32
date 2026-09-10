# OpenPit32 web app: Blazor WebAssembly, published as static files and
# served by nginx. Build from the repo root: docker build -f docker/web.Dockerfile .
FROM mcr.microsoft.com/dotnet/sdk:10.0 AS build
WORKDIR /src
COPY OpenPit32/ ./OpenPit32/
WORKDIR /src/OpenPit32
# DOCKER_DEPLOY makes Program.cs route API calls through nginx's own /api/
# reverse proxy (docker/nginx.conf) instead of the local-dev default of a
# separate port on the same host.
RUN dotnet publish -c Release -o /app/publish -p:DefineConstants=DOCKER_DEPLOY

FROM nginx:alpine
COPY --from=build /app/publish/wwwroot /usr/share/nginx/html
# A *.template here, not a plain .conf: nginx:alpine's entrypoint runs
# envsubst over everything in /etc/nginx/templates/ before startup,
# substituting ${GRILL_PROXY_HOST} (docker-compose.yml passes it through)
# and writing the result to /etc/nginx/conf.d/default.conf.
COPY docker/nginx.conf.template /etc/nginx/templates/default.conf.template
EXPOSE 80
