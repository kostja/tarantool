ARG BASE_IMAGE
FROM ${BASE_IMAGE}centos:7

ARG TNT_VER \
    GC64

ENV GC64=${GC64:-false} \
    LUAROCK_VSHARD_VERSION=0.1.18 \
    LUAROCK_AVRO_SCHEMA_VERSION=3.0.6 \
    LUAROCK_EXPERATIOND_VERSION=1.1.1 \
    LUAROCK_QUEUE_VERSION=1.1.0 \
    LUAROCK_CONNPOOL_VERSION=1.1.1 \
    LUAROCK_HTTP_VERSION=1.2.0 \
    LUAROCK_MEMCACHED_VERSION=1.0.1 \
    LUAROCK_METRICS_VERSION=0.12.0 \
    LUAROCK_TARANTOOL_PG_VERSION=2.0.2 \
    LUAROCK_TARANTOOL_MYSQL_VERSION=2.1.0 \
    LUAROCK_TARANTOOL_GIS_VERSION=1.0.0 \
    LUAROCK_TARANTOOL_PROMETHEUS_VERSION=1.0.4 \
    LUAROCK_TARANTOOL_GPERFTOOLS_VERSION=1.0.1

COPY files/luarocks-config_centos.lua /usr/local/etc/luarocks/config-5.1.lua
COPY files/luarocks-config.lua /usr/local/etc/tarantool/rocks/config-5.1.lua

RUN ["groupadd", "tarantool"]
RUN ["adduser", "-g", "tarantool", "tarantool"]

# Set yum repository for Postgresql 9.6 since this version
# has been removed from pgdg repository.
RUN echo $'[pg]\n\
name=PostgreSQL 9.6 RHEL/CentOS $releasever - $basearch\n\
baseurl=https://download.postgresql.org/pub/repos/yum/9.6/redhat/rhel-7-$basearch\n\
enabled=1\n\
gpgcheck=1\n\
gpgkey=https://download.postgresql.org/pub/repos/yum/RPM-GPG-KEY-PGDG-96\n\
repo_gpgcheck = 1\n'\
>> /etc/yum.repos.d/pg.repo \
    && yum install -y https://download.picodata.io/tarantool-picodata/el/7/x86_64/picodata-release-1.0.0-1.el7.x86_64.rpm \
    && yum -y install epel-release \
    && : "---------- tarantool ----------" \
    && yum install -y  \
        tarantool \
        tarantool-devel \
        mariadb-libs \
        postgresql96-libs \
        cyrus-sasl \
        libev \
        proj \
        geos \
        unzip \
        openssl-libs \
        luarocks \
        git \
        cmake \
        make \
        gcc-c++ \
        postgresql96-devel \
        cyrus-sasl-devel \
        libev-devel \
        wget \
        proj-devel \
        geos-devel \
        openssl-devel \
        gperftools-libs \
    && ln -s /usr/lib64/libprofiler.so.0 /usr/lib64/libprofiler.so \
    && mkdir -p /rocks \
    && tarantoolctl rocks install lua-term \
    && tarantoolctl rocks install vshard $LUAROCK_VSHARD_VERSION \
    && tarantoolctl rocks install checks $LUAROCK_CHECKS_VERSION \
    && tarantoolctl rocks install avro-schema $LUAROCK_AVRO_SCHEMA_VERSION \
    && tarantoolctl rocks install expirationd $LUAROCK_EXPERATIOND_VERSION \
    && tarantoolctl rocks install queue $LUAROCK_QUEUE_VERSION \
    && tarantoolctl rocks install connpool $LUAROCK_CONNPOOL_VERSION \
    && tarantoolctl rocks install http $LUAROCK_HTTP_VERSION \
    && tarantoolctl rocks install pg $LUAROCK_TARANTOOL_PG_VERSION \
    && tarantoolctl rocks install mysql $LUAROCK_TARANTOOL_MYSQL_VERSION \
    && tarantoolctl rocks install memcached $LUAROCK_MEMCACHED_VERSION \
    && tarantoolctl rocks install metrics $LUAROCK_METRICS_VERSION \
    && tarantoolctl rocks install prometheus $LUAROCK_TARANTOOL_PROMETHEUS_VERSION \
    && tarantoolctl rocks install gis $LUAROCK_TARANTOOL_GIS_VERSION \
    && tarantoolctl rocks install gperftools $LUAROCK_TARANTOOL_GPERFTOOLS_VERSION \
    && : "---------- remove build deps ----------" \
    && rm -rf /rocks \
    && yum -y remove \
        git \
        cmake \
        postgresql96-devel \
        cyrus-sasl-devel \
        libev-devel \
        wget \
        proj-devel \
        geos-devel \
        openssl-devel \
        kernel-headers \
        cpp \
        perl \
    && rm -rf /var/cache/yum \
    && : "---------- gosu ----------" \
    && gpg --keyserver hkp://keyserver.ubuntu.com:80 --recv-keys \
       B42F6819007F00F88E364FD4036A9C25BF357DD4 \
    && curl -o /usr/local/bin/gosu -SL \
       "https://github.com/tianon/gosu/releases/download/1.2/gosu-amd64" \
    && curl -o /usr/local/bin/gosu.asc -SL \
       "https://github.com/tianon/gosu/releases/download/1.2/gosu-amd64.asc" \
    && gpg --verify /usr/local/bin/gosu.asc \
    && rm /usr/local/bin/gosu.asc \
    && rm -r /root/.gnupg/ \
    && chmod +x /usr/local/bin/gosu \
    && mkdir -p /var/lib/tarantool \
    && chown tarantool:tarantool /var/lib/tarantool \
    && mkdir -p /opt/tarantool \
    && chown tarantool:tarantool /opt/tarantool \
    && mkdir -p /var/run/tarantool \
    && chown tarantool:tarantool /var/run/tarantool \
    && chown tarantool:tarantool /etc/tarantool \
    && ln -s usr/local/bin/docker-entrypoint.sh /entrypoint.sh

VOLUME /var/lib/tarantool
WORKDIR /opt/tarantool

COPY files/tarantool-entrypoint.lua /usr/local/bin/
COPY files/tarantool_set_config.lua /usr/local/bin/
COPY files/docker-entrypoint_centos.sh /usr/local/bin/docker-entrypoint.sh
COPY files/console /usr/local/bin/
COPY files/tarantool_is_up /usr/local/bin/
COPY files/tarantool.default /usr/local/etc/default/tarantool

ENTRYPOINT ["docker-entrypoint.sh"]

HEALTHCHECK CMD tarantool_is_up

EXPOSE 3301
CMD [ "tarantool" ]
