// Transport registry: one singleton per TransferMethod.
#include "net/net_internal.h"

namespace ks::net {

namespace detail {
Transport& webdavTransport();
Transport& crosspointHttpTransport();
Transport& crosspointWsTransport();
Transport& folderTransport();
Transport& bleMockTransport();
} // namespace detail

Transport& transport(TransferMethod m) {
    switch (m) {
        case TransferMethod::WebDav: return detail::webdavTransport();
        case TransferMethod::CrossPointHttp: return detail::crosspointHttpTransport();
        case TransferMethod::CrossPointWs: return detail::crosspointWsTransport();
        case TransferMethod::Folder: return detail::folderTransport();
        case TransferMethod::Ble: return detail::bleMockTransport();
    }
    return detail::webdavTransport();
}

std::vector<TransferMethod> methods() {
    return {TransferMethod::WebDav, TransferMethod::CrossPointHttp, TransferMethod::CrossPointWs,
            TransferMethod::Folder, TransferMethod::Ble};
}

} // namespace ks::net
