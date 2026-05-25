/****************************************************************************
**
** Copyright (C) 2012 Denis Shienkov <denis.shienkov@gmail.com>
** Copyright (C) 2012 Laszlo Papp <lpapp@kde.org>
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtSerialPort module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:BSD$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** BSD License Usage
** Alternatively, you may use this file under the terms of the BSD license
** as follows:
**
** "Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are
** met:
**   * Redistributions of source code must retain the above copyright
**     notice, this list of conditions and the following disclaimer.
**   * Redistributions in binary form must reproduce the above copyright
**     notice, this list of conditions and the following disclaimer in
**     the documentation and/or other materials provided with the
**     distribution.
**   * Neither the name of The Qt Company Ltd nor the names of its
**     contributors may be used to endorse or promote products derived
**     from this software without specific prior written permission.
**
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
** LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
** A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
** OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
** LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
** OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "include/settingsdialog.h"
#include "ui_settingsdialog.h"

#include <QIntValidator>
#include <QLineEdit>
#include <QSerialPortInfo>
#include <QListWidget>
#include <QStandardItemModel>
#include <QSignalBlocker>
#include <QMessageBox>

static const char blankString[] = QT_TRANSLATE_NOOP("SettingsDialog", "N/A");

SettingsDialog::SettingsDialog(QWidget *parent) :
    QDialog(parent),
    m_ui(new Ui::SettingsDialog)
{
    m_ui->setupUi(this);

    m_ui->baudRateBox->setInsertPolicy(QComboBox::NoInsert);
    m_ui->control_baudRateBox->setInsertPolicy(QComboBox::NoInsert);

    connect(m_ui->applyButton, &QPushButton::clicked,
            this, &SettingsDialog::apply);
    connect(m_ui->serialPortInfoListBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SettingsDialog::showPortInfo);
    connect(m_ui->control_serialPortInfoListBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SettingsDialog::showControlPortInfo);
    connect(m_ui->closeButton, &QPushButton::clicked,
            this, &SettingsDialog::hide);

    // Нельзя выбрать один и тот же порт в обоих комбобоксах.
    connect(m_ui->serialPortInfoListBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SettingsDialog::syncPortExclusion);
    connect(m_ui->control_serialPortInfoListBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SettingsDialog::syncPortExclusion);

    fillPortsParameters();

    int availPortsCount = fillPortsInfo();
    m_ui->applyButton->setEnabled((0 < availPortsCount) ? true : false);

    loadParamFromSettings();
    syncPortExclusion();
    updateSettings();
}

SettingsDialog::~SettingsDialog()
{
    delete m_ui;
}

SettingsDialog::Settings SettingsDialog::settings() const
{
    return m_currentSettings;
}

SettingsDialog::Settings SettingsDialog::controlSettings() const
{
    return m_currentControlSettings;
}

void SettingsDialog::showPortInfo(int idx)
{
    if (idx == -1)
        return;

    const QStringList list = m_ui->serialPortInfoListBox->itemData(idx).toStringList();
    m_ui->descriptionLabel->setText(tr("Description: %1").arg(list.count() > 1 ? list.at(1) : tr(blankString)));
    m_ui->manufacturerLabel->setText(tr("Manufacturer: %1").arg(list.count() > 2 ? list.at(2) : tr(blankString)));
}

void SettingsDialog::showControlPortInfo(int idx)
{
    if (idx == -1)
        return;

    const QStringList list = m_ui->control_serialPortInfoListBox->itemData(idx).toStringList();
    m_ui->control_descriptionLabel->setText(tr("Description: %1").arg(list.count() > 1 ? list.at(1) : tr(blankString)));
    m_ui->control_manufacturerLabel->setText(tr("Manufacturer: %1").arg(list.count() > 2 ? list.at(2) : tr(blankString)));
}

void SettingsDialog::apply()
{
    // Страховка на случай, если у юзера всего один порт в системе и он
    // случайно совпал. Само UI не даст выбрать, но на всякий случай.
    if (m_ui->serialPortInfoListBox->count() > 1 &&
        !m_ui->serialPortInfoListBox->currentText().isEmpty() &&
        m_ui->serialPortInfoListBox->currentText() ==
        m_ui->control_serialPortInfoListBox->currentText())
    {
        QMessageBox::warning(this, tr("Settings"),
                             tr("Терминальный и управляющий порты должны отличаться."));
        return;
    }

    updateSettings();
    //hide();

    emit settingsApplied(true);
}

// Делает пункт в box'е (не)активным.
static void setComboItemEnabled(QComboBox* box, int row, bool enabled)
{
    auto* model = qobject_cast<QStandardItemModel*>(box->model());
    if (!model) return;
    QStandardItem* item = model->item(row);
    if (!item) return;

    Qt::ItemFlags f = item->flags();
    if (enabled) f |=  Qt::ItemIsEnabled;
    else         f &= ~Qt::ItemIsEnabled;
    item->setFlags(f);
}

static void applyExclusion(QComboBox* target, const QString& excludedText)
{
    for (int i = 0; i < target->count(); ++i) {
        const bool disable = (!excludedText.isEmpty() && target->itemText(i) == excludedText);
        setComboItemEnabled(target, i, !disable);
    }
    // Если текущий выбранный пункт стал недоступен — переключаемся на первый доступный.
    const QString cur = target->currentText();
    if (!excludedText.isEmpty() && cur == excludedText) {
        for (int i = 0; i < target->count(); ++i) {
            if (target->itemText(i) != excludedText) {
                QSignalBlocker b(target);
                target->setCurrentIndex(i);
                break;
            }
        }
    }
}

void SettingsDialog::syncPortExclusion()
{
    if (m_syncingExclusion) return;
    m_syncingExclusion = true;

    const QString t = m_ui->serialPortInfoListBox->currentText();
    const QString c = m_ui->control_serialPortInfoListBox->currentText();

    applyExclusion(m_ui->control_serialPortInfoListBox, t);
    applyExclusion(m_ui->serialPortInfoListBox, c);

    m_syncingExclusion = false;
}

void SettingsDialog::fillPortsParameters()
{
    // --- Терминальный порт ---
    m_ui->baudRateBox->addItem(QStringLiteral("9600"), QSerialPort::Baud9600);
    m_ui->baudRateBox->addItem(QStringLiteral("19200"), QSerialPort::Baud19200);
    m_ui->baudRateBox->addItem(QStringLiteral("38400"), QSerialPort::Baud38400);
    m_ui->baudRateBox->addItem(QStringLiteral("115200"), QSerialPort::Baud115200);
    m_ui->baudRateBox->addItem(QStringLiteral("230400"), 230400);

    m_ui->dataBitsBox->addItem(QStringLiteral("5"), QSerialPort::Data5);
    m_ui->dataBitsBox->addItem(QStringLiteral("6"), QSerialPort::Data6);
    m_ui->dataBitsBox->addItem(QStringLiteral("7"), QSerialPort::Data7);
    m_ui->dataBitsBox->addItem(QStringLiteral("8"), QSerialPort::Data8);
    m_ui->dataBitsBox->setCurrentIndex(3);

    m_ui->parityBox->addItem(tr("None"), QSerialPort::NoParity);
    m_ui->parityBox->addItem(tr("Even"), QSerialPort::EvenParity);
    m_ui->parityBox->addItem(tr("Odd"), QSerialPort::OddParity);
    m_ui->parityBox->addItem(tr("Mark"), QSerialPort::MarkParity);
    m_ui->parityBox->addItem(tr("Space"), QSerialPort::SpaceParity);

    m_ui->stopBitsBox->addItem(QStringLiteral("1"), QSerialPort::OneStop);
#ifdef Q_OS_WIN
    m_ui->stopBitsBox->addItem(tr("1.5"), QSerialPort::OneAndHalfStop);
#endif
    m_ui->stopBitsBox->addItem(QStringLiteral("2"), QSerialPort::TwoStop);

    m_ui->flowControlBox->addItem(tr("None"), QSerialPort::NoFlowControl);
    m_ui->flowControlBox->addItem(tr("RTS/CTS"), QSerialPort::HardwareControl);
    m_ui->flowControlBox->addItem(tr("XON/XOFF"), QSerialPort::SoftwareControl);

    // --- Управляющий порт (VKA non-debug, дефолт 115200) ---
    m_ui->control_baudRateBox->addItem(QStringLiteral("9600"), QSerialPort::Baud9600);
    m_ui->control_baudRateBox->addItem(QStringLiteral("19200"), QSerialPort::Baud19200);
    m_ui->control_baudRateBox->addItem(QStringLiteral("38400"), QSerialPort::Baud38400);
    m_ui->control_baudRateBox->addItem(QStringLiteral("115200"), QSerialPort::Baud115200);
    m_ui->control_baudRateBox->addItem(QStringLiteral("230400"), 230400);
    m_ui->control_baudRateBox->setCurrentIndex(3); // 115200 по умолчанию

    m_ui->control_dataBitsBox->addItem(QStringLiteral("5"), QSerialPort::Data5);
    m_ui->control_dataBitsBox->addItem(QStringLiteral("6"), QSerialPort::Data6);
    m_ui->control_dataBitsBox->addItem(QStringLiteral("7"), QSerialPort::Data7);
    m_ui->control_dataBitsBox->addItem(QStringLiteral("8"), QSerialPort::Data8);
    m_ui->control_dataBitsBox->setCurrentIndex(3);

    m_ui->control_parityBox->addItem(tr("None"), QSerialPort::NoParity);
    m_ui->control_parityBox->addItem(tr("Even"), QSerialPort::EvenParity);
    m_ui->control_parityBox->addItem(tr("Odd"), QSerialPort::OddParity);
    m_ui->control_parityBox->addItem(tr("Mark"), QSerialPort::MarkParity);
    m_ui->control_parityBox->addItem(tr("Space"), QSerialPort::SpaceParity);

    m_ui->control_stopBitsBox->addItem(QStringLiteral("1"), QSerialPort::OneStop);
#ifdef Q_OS_WIN
    m_ui->control_stopBitsBox->addItem(tr("1.5"), QSerialPort::OneAndHalfStop);
#endif
    m_ui->control_stopBitsBox->addItem(QStringLiteral("2"), QSerialPort::TwoStop);

    m_ui->control_flowControlBox->addItem(tr("None"), QSerialPort::NoFlowControl);
    m_ui->control_flowControlBox->addItem(tr("RTS/CTS"), QSerialPort::HardwareControl);
    m_ui->control_flowControlBox->addItem(tr("XON/XOFF"), QSerialPort::SoftwareControl);
}

int SettingsDialog::fillPortsInfo()
{
    m_ui->serialPortInfoListBox->clear();
    m_ui->control_serialPortInfoListBox->clear();

    QString description;
    QString manufacturer;
    QString serialNumber;
    const auto infos = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &info : infos) {
        QStringList list;
        description = info.description();
        manufacturer = info.manufacturer();
        serialNumber = info.serialNumber();
        list << info.portName()
             << (!description.isEmpty() ? description : blankString)
             << (!manufacturer.isEmpty() ? manufacturer : blankString)
             << (!serialNumber.isEmpty() ? serialNumber : blankString)
             << info.systemLocation()
             << (info.vendorIdentifier() ? QString::number(info.vendorIdentifier(), 16) : blankString)
             << (info.productIdentifier() ? QString::number(info.productIdentifier(), 16) : blankString);

        m_ui->serialPortInfoListBox->addItem(list.first(), list);
        m_ui->control_serialPortInfoListBox->addItem(list.first(), list);
    }

    return infos.count();
}

void SettingsDialog::updateSettings()
{
    // --- Терминальный порт ---
    m_currentSettings.name = m_ui->serialPortInfoListBox->currentText();

    m_currentSettings.baudRate = static_cast<QSerialPort::BaudRate>(
                m_ui->baudRateBox->itemData(m_ui->baudRateBox->currentIndex()).toInt());

    m_currentSettings.stringBaudRate = QString::number(m_currentSettings.baudRate);

    m_currentSettings.dataBits = static_cast<QSerialPort::DataBits>(
                m_ui->dataBitsBox->itemData(m_ui->dataBitsBox->currentIndex()).toInt());
    m_currentSettings.stringDataBits = m_ui->dataBitsBox->currentText();

    m_currentSettings.parity = static_cast<QSerialPort::Parity>(
                m_ui->parityBox->itemData(m_ui->parityBox->currentIndex()).toInt());
    m_currentSettings.stringParity = m_ui->parityBox->currentText();

    m_currentSettings.stopBits = static_cast<QSerialPort::StopBits>(
                m_ui->stopBitsBox->itemData(m_ui->stopBitsBox->currentIndex()).toInt());
    m_currentSettings.stringStopBits = m_ui->stopBitsBox->currentText();

    m_currentSettings.flowControl = static_cast<QSerialPort::FlowControl>(
                m_ui->flowControlBox->itemData(m_ui->flowControlBox->currentIndex()).toInt());
    m_currentSettings.stringFlowControl = m_ui->flowControlBox->currentText();

    m_currentSettings.localEchoEnabled = m_ui->localEchoCheckBox->isChecked();

    // --- Управляющий порт ---
    m_currentControlSettings.name = m_ui->control_serialPortInfoListBox->currentText();

    m_currentControlSettings.baudRate = static_cast<QSerialPort::BaudRate>(
                m_ui->control_baudRateBox->itemData(m_ui->control_baudRateBox->currentIndex()).toInt());
    m_currentControlSettings.stringBaudRate = QString::number(m_currentControlSettings.baudRate);

    m_currentControlSettings.dataBits = static_cast<QSerialPort::DataBits>(
                m_ui->control_dataBitsBox->itemData(m_ui->control_dataBitsBox->currentIndex()).toInt());
    m_currentControlSettings.stringDataBits = m_ui->control_dataBitsBox->currentText();

    m_currentControlSettings.parity = static_cast<QSerialPort::Parity>(
                m_ui->control_parityBox->itemData(m_ui->control_parityBox->currentIndex()).toInt());
    m_currentControlSettings.stringParity = m_ui->control_parityBox->currentText();

    m_currentControlSettings.stopBits = static_cast<QSerialPort::StopBits>(
                m_ui->control_stopBitsBox->itemData(m_ui->control_stopBitsBox->currentIndex()).toInt());
    m_currentControlSettings.stringStopBits = m_ui->control_stopBitsBox->currentText();

    m_currentControlSettings.flowControl = static_cast<QSerialPort::FlowControl>(
                m_ui->control_flowControlBox->itemData(m_ui->control_flowControlBox->currentIndex()).toInt());
    m_currentControlSettings.stringFlowControl = m_ui->control_flowControlBox->currentText();

    // для управляющего порта локальное эхо не используется
    m_currentControlSettings.localEchoEnabled = false;
}

static void restoreComboByData(QComboBox *box, const QVariant &value)
{
    if (!value.isValid()) return;
    int index = box->findData(value);
    if (index != -1)
        box->setCurrentIndex(index);
}

static void restoreComboByText(QComboBox *box, const QString &value)
{
    if (value.isEmpty()) return;
    int index = box->findText(value, Qt::MatchContains);
    if (index != -1)
        box->setCurrentIndex(index);
}

void SettingsDialog::loadParamFromSettings()
{
    QSettings settings("MySoft", "terminal2");

    // --- Терминальный порт ---
    settings.beginGroup("SerialSettings");
    restoreComboByText(m_ui->serialPortInfoListBox, settings.value("name").toString());
    restoreComboByData(m_ui->baudRateBox,    settings.value("baudRate"));
    restoreComboByData(m_ui->dataBitsBox,    settings.value("dataBits"));
    restoreComboByData(m_ui->parityBox,      settings.value("parity"));
    restoreComboByData(m_ui->stopBitsBox,    settings.value("stopBits"));
    restoreComboByData(m_ui->flowControlBox, settings.value("flowControl"));
    m_ui->localEchoCheckBox->setChecked(settings.value("localEchoEnabled").toBool());
    settings.endGroup();

    // --- Управляющий порт ---
    settings.beginGroup("ControlSerialSettings");
    restoreComboByText(m_ui->control_serialPortInfoListBox, settings.value("name").toString());
    restoreComboByData(m_ui->control_baudRateBox,    settings.value("baudRate"));
    restoreComboByData(m_ui->control_dataBitsBox,    settings.value("dataBits"));
    restoreComboByData(m_ui->control_parityBox,      settings.value("parity"));
    restoreComboByData(m_ui->control_stopBitsBox,    settings.value("stopBits"));
    restoreComboByData(m_ui->control_flowControlBox, settings.value("flowControl"));
    settings.endGroup();
}
