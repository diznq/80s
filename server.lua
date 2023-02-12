function on_data(epollfd, childfd, data, len)
    print("Received ", data)
    net_write(epollfd, childfd, "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-length: 5\r\n\r\nHello", true)
end