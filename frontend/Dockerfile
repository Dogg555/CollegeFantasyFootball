# Simple static site container using nginx
FROM nginx:1.30.4-alpine-slim@sha256:ddde39c6e51f02fde7410c2e9c234cf2d0a4c7bdbbe176aeb37d8ad7ab4eb58c
WORKDIR /usr/share/nginx/html
COPY . /usr/share/nginx/html
COPY nginx.conf /etc/nginx/conf.d/default.conf
RUN chown -R nginx:nginx \
      /usr/share/nginx/html \
      /var/cache/nginx \
      /var/run \
      /etc/nginx/conf.d
USER nginx
EXPOSE 8080
