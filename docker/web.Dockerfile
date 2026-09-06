# OpenPit32 web app: Blazor WebAssembly, published as static files and
# served by nginx. Build from the repo root: docker build -f docker/web.Dockerfile .
FROM mcr.microsoft.com/dotnet/sdk:10.0 AS build
WORKDIR /src
COPY OpenPit32/ ./OpenPit32/
WORKDIR /src/OpenPit32
RUN dotnet publish -c Release -o /app/publish
# Route the app's API calls through nginx's own /api/ reverse proxy (see
# docker/nginx.conf) instead of the local-dev default of a separate port on
# the same host — Program.cs falls back to that default when this file is
# absent, which is the case for a plain `dotnet run`.
RUN echo '{"SidecarBaseUrl": "/api/"}' > /app/publish/wwwroot/appsettings.json

FROM nginx:alpine
COPY --from=build /app/publish/wwwroot /usr/share/nginx/html
COPY docker/nginx.conf /etc/nginx/conf.d/default.conf
EXPOSE 80
