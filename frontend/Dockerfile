# Simple static site container using nginx
FROM nginx:1.28-alpine-slim
WORKDIR /usr/share/nginx/html
COPY . /usr/share/nginx/html
COPY nginx.conf /etc/nginx/conf.d/default.conf
EXPOSE 80
