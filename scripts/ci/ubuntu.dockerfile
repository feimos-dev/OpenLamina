FROM ubuntu:noble
RUN dpkg --add-architecture i386 && \
	apt-get update && \
	apt-get install -y --no-install-recommends \
		g++ \
		g++-multilib \
		cmake \
		make \
		automake \
		autoconf \
		libtool \
		python3 \
		libgl-dev \
		libvulkan-dev \
		libgl-dev:i386 \
		libvulkan-dev:i386 \
		xvfb \
		xauth \
		mesa-vulkan-drivers \
		libgl1-mesa-dri \
		libglx-mesa0 \
		mesa-vulkan-drivers:i386 \
		libgl1-mesa-dri:i386 \
		libglx-mesa0:i386 && \
	rm -r /var/lib/apt/lists/*
