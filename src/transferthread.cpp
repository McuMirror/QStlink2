/*
This file is part of QSTLink2.

    QSTLink2 is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    QSTLink2 is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with QSTLink2.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "transferthread.h"

transferThread::transferThread(QObject *parent) :
    QThread(parent)
{
    qDebug() << "New Transfer Thread";
    this->m_stop = false;
}

void transferThread::run()
{
    if (this->m_write) {
        this->send(this->m_filename);
        if (this->m_verify)
            this->verify(this->m_filename);
    }
    else if (!this->m_verify) {
        this->receive(this->m_filename);
    }
    else {
        this->verify(this->m_filename);
    }
}
void transferThread::halt()
{
    this->m_stop = true;
    emit sendStatus("Aborted");
    emit sendLog("Transfer Aborted");
}

void transferThread::setParams(stlinkv2 *stlink, QString filename, bool write, bool erase, bool verify)
{
    this->stlink = stlink;
    this->m_filename = filename;
    this->m_write = write;
    this->m_erase = erase;
    this->m_verify = verify;
}

void transferThread::send(const QString &filename)
{
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly)) {
        qCritical("Could not open the file.");
        return;
    }
    emit sendLock(true);
    this->m_stop = false;
    this->stlink->hardResetMCU(); // We stop the MCU
    const quint8 program_size = 4; // WORD (4 bytes)
    // The MCU will write program_size 32 times to empty the MCU buffer.
    // We don't have to write to USB only a few bytes at a time, the MCU handles flash programming for us.
    quint32 step_size = program_size*32;
    quint32 from = (*this->stlink->device)["flash_base"];
    quint32 to = (*this->stlink->device)["flash_base"]+file.size();
    qInformal() << "Writing from" << "0x"+QString::number(from, 16) << "to" << "0x"+QString::number(to, 16);
    QByteArray buf;
    quint32 addr, progress, oldprogress, read;
    char *buf2 = new char[step_size];

    this->stlink->isLocked();

    // Unlock flash
    if (!this->stlink->unlockFlash())
        return;

    // Does not work on F0
//    this->stlink->setProgramSize(program_size);
    this->stlink->setProgramSize(2);

    while(this->stlink->isBusy()) {
        usleep(10000); // 100ms
    }

    if (this->m_erase) {
        emit sendStatus("Erasing flash... This might take some time.");

        if (!this->stlink->eraseFlash()) {
            qCritical() << "eraseFlash Failed!";
            return;
        }
    }

    this->stlink->resetMCU();

    // We finally enable flash programming
    if (!this->stlink->setFlashProgramming(true)) {
        qCritical() << "setFlashProgramming Failed!";
        return;
    }

    while(this->stlink->isBusy()) {
        usleep(100000); // 1000ms
    }

    // Unlock flash again. Seems like this is mandatory.
    if (!this->stlink->unlockFlash())
        return;

    progress = 0;
    for (int i=0; i<=file.size(); i+=step_size)
    {
        buf.clear();

        if (this->m_stop)
            break;

        if (file.atEnd()) {
            qDebug() << "End Of File";
            break;
        }

        memset(buf2, 0, step_size);
        if ((read = file.read(buf2, step_size)) <= 0)
            break;
        qDebug() << "Read" << read << "Bytes from disk";
        buf.append(buf2, read);

        addr = (*this->stlink->device)["flash_base"]+i;
        this->stlink->writeMem32(addr, buf);
        oldprogress = progress;
        progress = (i*100)/file.size();
        if (progress > oldprogress) { // Push only if number has increased
            emit sendProgress(progress);
            qInformal() << "Progress:"<< QString::number(progress)+"%";
        }
        emit sendStatus("Transfered "+QString::number(i/1024)+" kilobytes out of "+QString::number(file.size()/1024));
    }
    file.close();
    delete buf2;

    emit sendProgress(100);
    emit sendStatus("Transfer done");
    emit sendLog("Transfer done");
    qInformal() << "Transfer done";

//    // We disable flash programming
//    if (this->stlink->setFlashProgramming(false))
//        return;

//    // Lock flash
//    this->stlink->lockFlash();

    this->stlink->hardResetMCU();
    this->stlink->resetMCU();
    this->stlink->runMCU();

    emit sendLock(false);
}

void transferThread::receive(const QString &filename)
{
    QFile file(filename);
    if (!file.open(QIODevice::ReadWrite)) {
        qCritical("Could not save the file.");
        return;
    }
    emit sendLock(true);
    this->m_stop = false;
    this->stlink->hardResetMCU(); // We stop the MCU
    quint32 buf_size = (*this->stlink->device)["flash_pgsize"]*4;
    quint32 from = (*this->stlink->device)["flash_base"];
    quint32 to = (*this->stlink->device)["flash_base"]+from;
    qInformal() << "Reading from" << QString::number(from, 16) << "to" << QString::number(to, 16);
    quint32 addr, progress, oldprogress;

    progress = 0;
    for (quint32 i=0; i<(*this->stlink->device)["flash_size"]; i+=buf_size)
    {
        if (this->m_stop)
            break;
        addr = (*this->stlink->device)["flash_base"]+i;
        if (this->stlink->readMem32(addr, buf_size) < 0)
            break;
        qDebug() << "Wrote" << file.write(this->stlink->recv_buf) << "Bytes to disk";
        oldprogress = progress;
        progress = (i*100)/(*this->stlink->device)["flash_size"];
        if (progress > oldprogress) { // Push only if number has increased
            emit sendProgress(progress);
            qInformal() << "Progress:"<< QString::number(progress)+"%";
        }
        emit sendStatus("Transfered "+QString::number(i/1024)+" kilobytes out of "+QString::number((*this->stlink->device)["flash_size"]/1024));
    }
    file.close();
    emit sendProgress(100);
    emit sendStatus("Transfer done");
    qInformal() << "Transfer done";
    this->stlink->runMCU();
    emit sendLock(false);
}

void transferThread::verify(const QString &filename)
{
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly)) {
        qCritical("Could not save the file.");
        return;
    }
    emit sendLock(true);
    this->m_stop = false;
    this->stlink->hardResetMCU(); // We stop the MCU
    quint32 buf_size = (*this->stlink->device)["flash_pgsize"]*4;
    quint32 from = (*this->stlink->device)["flash_base"];
    quint32 to = (*this->stlink->device)["flash_base"]+file.size();
    qInformal() << "Reading from" << QString::number(from, 16) << "to" << QString::number(to, 16);
    quint32 addr, progress, oldprogress;
    QByteArray tmp;

    progress = 0;
    for (quint32 i=0; i<file.size(); i+=buf_size)
    {
        if (this->m_stop)
            break;
        addr = (*this->stlink->device)["flash_base"]+i;
        if (this->stlink->readMem32(addr, buf_size) < 0)
            break;

        tmp = file.read(buf_size);
        if (this->stlink->recv_buf != tmp) {

            file.close();
            emit sendProgress(100);
            emit sendStatus("Verification failed!");
            qCritical() << "Verification failed";
            this->stlink->runMCU();
            emit sendLock(false);
            return;
        }
        oldprogress = progress;
        progress = (i*100)/(*this->stlink->device)["flash_size"];
        if (progress > oldprogress) { // Push only if number has increased
            emit sendProgress(progress);
            qInformal() << "Progress:"<< QString::number(progress)+"%";
        }
        emit sendStatus("Verified "+QString::number(i/1024)+" kilobytes out of "+QString::number((*this->stlink->device)["flash_size"]/1024));
    }
    file.close();
    emit sendProgress(100);
    emit sendStatus("Verification done");
    qInformal() << "Verification done";
    this->stlink->runMCU();
    emit sendLock(false);
}
