FROM dokken/centos-stream-10

ARG TOOLCHAIN_VERSION

RUN dnf install -y wget git
# Do NOT be smart here and clean the cache because the container is used in the
# stateful context.

RUN wget -q https://s3-eu-west-1.amazonaws.com/deps.memgraph.io/toolchain-${TOOLCHAIN_VERSION}/toolchain-${TOOLCHAIN_VERSION}-binaries-centos-10-x86_64.tar.gz \
    -O toolchain-${TOOLCHAIN_VERSION}-binaries-centos-10-x86_64.tar.gz \
    && tar xzvf toolchain-${TOOLCHAIN_VERSION}-binaries-centos-10-x86_64.tar.gz -C /opt \
    && rm toolchain-${TOOLCHAIN_VERSION}-binaries-centos-10-x86_64.tar.gz

# Install toolchain run deps and memgraph build deps
ARG GIT_REF
SHELL ["/bin/bash", "-c"]
RUN git clone --branch ${GIT_REF} https://github.com/memgraph/memgraph.git \
    && cd memgraph \
    && ./environment/os/centos-10.sh install TOOLCHAIN_RUN_DEPS \
    && ./environment/os/centos-10.sh install MEMGRAPH_BUILD_DEPS \
    && cd .. && rm -rf memgraph

# Create mg user and set as default
RUN useradd -m -s /bin/bash mg
USER mg

# Install rust
ARG RUST_VERSION
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y \
    && . "$HOME/.cargo/env" \
    && rustup default ${RUST_VERSION}

# Fix node
ARG NODE_VERSION
RUN curl https://raw.githubusercontent.com/creationix/nvm/master/install.sh | bash \
    && . ~/.nvm/nvm.sh \
    && nvm install ${NODE_VERSION} \
    && nvm use ${NODE_VERSION}

# Install PyYAML (only for amzn-2, centos-7, cento-9 and rocky-9)
RUN pip3 install --user PyYAML

ENTRYPOINT ["sleep", "infinity"]
