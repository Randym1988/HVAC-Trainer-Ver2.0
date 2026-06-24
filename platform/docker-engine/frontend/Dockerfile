FROM nginx:alpine

# Copy reorganized frontend files
COPY web/instructor/index.html /usr/share/nginx/html/index.html
COPY web/student/student.html /usr/share/nginx/html/student.html

# Copy custom NGINX config to handle API proxying
COPY frontend/nginx.conf /etc/nginx/conf.d/default.conf

EXPOSE 80
CMD ["nginx", "-g", "daemon off;"]
