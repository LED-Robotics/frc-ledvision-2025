docker run --restart unless-stopped --name ledvision --net=host --privileged -v /home/robot/container-dir:/home/runner/shared-dir -v /dev/bus/usb:/dev/bus/usb -v /tmp/argus_socket:/tmp/argus_socket --runtime nvidia -d -t ledvision:latest bash
