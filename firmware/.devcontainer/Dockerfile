# Optional build container for the Dispenser V2 firmware (see firmware/README.md).
#
# Pinned to ESP-IDF v6.1 (= 6.1.0, the version used by the native setup) by tag AND
# immutable multi-arch index digest. The digest wins: if Espressif ever re-pushes the
# v6.1 tag, this file keeps pulling the image verified on 2026-11-15. To move to a newer
# ESP-IDF, update both values together and re-verify, e.g.
#   docker buildx imagetools inspect espressif/idf:<tag>
ARG IDF_IMAGE=espressif/idf:v6.1@sha256:81893c71bb5e570088901f21def8684c25cd2a9020281bd01b843a7655edb18c
FROM ${IDF_IMAGE}

ENV LC_ALL=C.UTF-8
ENV LANG=C.UTF-8

# VS Code terminals are started with `exec`, bypassing the image entrypoint that
# activates ESP-IDF, so activate it in interactive shells as well.
RUN echo "source /opt/esp/idf/export.sh > /dev/null 2>&1" >> ~/.bashrc

ENTRYPOINT [ "/opt/esp/entrypoint.sh" ]

CMD ["/bin/bash", "-c"]
