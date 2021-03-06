// Copyright (c) 2011-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_OPTIONSDIALOG_H
#define BITCOIN_QT_OPTIONSDIALOG_H

#include "walletmodel.h"

#include <QDialog>
#include <QString>
#include <QValidator>

class OptionsModel;
class WalletModel;
class ClientModel;
class QValidatedLineEdit;
class PlatformStyle;

QT_BEGIN_NAMESPACE
class QDataWidgetMapper;
QT_END_NAMESPACE

namespace Ui {
class OptionsDialog;
}

/** Proxy address widget validator, checks for a valid proxy address.
 */
class ProxyAddressValidator : public QValidator
{
    Q_OBJECT

public:
    explicit ProxyAddressValidator(QObject *parent);

    State validate(QString &input, int &pos) const;
};

/** Preferences dialog. */
class OptionsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit OptionsDialog(const PlatformStyle *platformStyle, QWidget *parent = 0 , bool enableWallet = true);
    ~OptionsDialog();

    void setModel(OptionsModel *model);
    void setWalletModel(WalletModel *walletModel);
    void setClientModel(ClientModel *clientModel);
    void setMapper();

private Q_SLOTS:
    /* set OK button state (enabled / disabled) */
    void setOkButtonState(bool fState);
    void on_resetButton_clicked();
    void on_okButton_clicked();
    void on_cancelButton_clicked();
    
    void on_hideTrayIcon_stateChanged(int fState);

    void showRestartWarning(bool fPersistent = false);
    void clearStatusLabel();
    void updateProxyValidationState();
    /* query the networks, for which the default proxy is used */
    void updateDefaultProxyNets();

Q_SIGNALS:
    void proxyIpChecks(QValidatedLineEdit *pUiProxyIp, int nProxyPort);

private:
    Ui::OptionsDialog *ui;
    OptionsModel *model;
    WalletModel *walletModel;
    ClientModel *clientModel;
    const PlatformStyle *platformStyle;
    QDataWidgetMapper *mapper;
};

#endif // BITCOIN_QT_OPTIONSDIALOG_H
