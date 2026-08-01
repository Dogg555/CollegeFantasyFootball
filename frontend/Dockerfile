# Simple static site container using nginx
FROM nginx:1.28-alpine-slim
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
