/*
Copyright (c) 2012, Daniel Moreno and Gabriel Taubin
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the Brown University nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL DANIEL MORENO AND GABRIEL TAUBIN BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#include "ui/TreeModel.h"

namespace smcp
{

TreeModel::TreeModel(QObject * parent) :
    QAbstractItemModel(parent),
    _columnCount(1),
    _horizontalHeader(QList<QString>()<<"Image"),
    _root()
{
}

TreeModel::TreeModel(unsigned columns, QObject * parent) :
    QAbstractItemModel(parent),
    _columnCount(columns),
    _root()
{
}

TreeModel::Item * TreeModel::GetItem(const QModelIndex & index)
{
    return reinterpret_cast<Item *>(index.internalPointer());
}

const TreeModel::Item * TreeModel::GetItem(const QModelIndex & index) const
{
    return const_cast<TreeModel *>(this)->GetItem(index);
}

QModelIndex TreeModel::index(int row, int column, const QModelIndex & parent) const
{
    const Item * item = GetItem(parent);
    if (!item)
    {
        item = &_root;
    }

    return createIndex(row, column, const_cast<Item *>(item->Child(row)));
}

QModelIndex TreeModel::parent(const QModelIndex & index) const
{
    const Item * item = GetItem(index);
    if (item)
    {
        const Item * parent = item->Parent();
        if (parent)
        {
            const Item * grandParent = parent->Parent();
            int row = 0;
            if (grandParent)
            {
                row = grandParent->ChildRow(parent);
            }
            return createIndex(row, 0, const_cast<Item *>(parent));
        }
    }
    return QModelIndex();
}

int TreeModel::rowCount(const QModelIndex & parent) const
{
    const Item * item = GetItem(parent);
    if (!item)
    {
        item = &_root;
    }
    return item->ChildrenCount();
}

int TreeModel::columnCount(const QModelIndex & parent) const
{
    return _columnCount;
}

QVariant TreeModel::data(const QModelIndex & index, int role) const
{
    const Item * item = GetItem(index);
    if (item)
    {
        return item->Data(role);
    }

    //not found
    return QVariant();
}

QVariant TreeModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation==Qt::Horizontal && role==Qt::DisplayRole && section<_horizontalHeader.size())
    {
        return _horizontalHeader.at(section);
    }
    return QVariant();
}

bool TreeModel::setData(const QModelIndex & index, const QVariant & value, int role)
{
    Item * item = GetItem(index);
    if (item)
    {
        item->SetData(value, role);
        emit dataChanged(index, index);
        return true;
    }

    //error
    return false;
}

bool TreeModel::InsertRow(int row, const QModelIndex & parent)
{
    Item * item = GetItem(parent);
    if (!item)
    {
        item = &_root;
    }

    beginInsertRows(parent, row, row);
    bool rv = item->InsertRow(row);
    endInsertRows();

    return rv;
}

Qt::ItemFlags TreeModel::flags(const QModelIndex & index) const
{
    if (!index.isValid())
    {
        return Qt::NoItemFlags;
    }
    if (!index.parent().parent().isValid())
    {
        return Qt::ItemIsEnabled|Qt::ItemIsSelectable|Qt::ItemIsUserCheckable;
    }
    return Qt::ItemIsEnabled|Qt::ItemIsSelectable;
}

void TreeModel::Clear(void)
{
    //removeRows(0, rowCount());
    beginResetModel();
    _root.Clear();
    endResetModel();
}

/***********************************
 *     TreeModel::Item Members     *
 ***********************************/

unsigned TreeModel::Item::nextId = 0;

TreeModel::Item::Item() : _id(nextId++), _parent(NULL), _data(), _children()
{
}

void TreeModel::Item::Clear(void)
{
    _children.clear();
    _data.clear();
}

bool TreeModel::Item::InsertRow(int row)
{
    if (row>=0 && row<=_children.size())
    {
        _children.insert(row, Item());
        _children[row].SetParent(this);
        return true;
    }
    return false;
}

void TreeModel::Item::SetData(const QVariant & value, int role)
{
    _data.insert(role, value);
}

QVariant TreeModel::Item::Data(int role) const
{
    QMap<int, QVariant>::const_iterator iter = _data.constFind(role);
    if (iter!=_data.constEnd())
    {
        return *iter;
    }
    return QVariant();
}

int TreeModel::Item::ChildrenCount(void) const
{
    return _children.size();
}

TreeModel::Item * TreeModel::Item::Child(int index)
{
    if (index>=0 && index<_children.size())
    {
        return &(_children[index]);
    }
    return NULL;
}

const TreeModel::Item * TreeModel::Item::Child(int index) const
{
    return const_cast<TreeModel::Item *>(this)->Child(index);
}

int TreeModel::Item::ChildRow(const TreeModel::Item * child) const
{
    if (!child)
    {
        return -1;
    }
    int index = 0;
    foreach(const TreeModel::Item & item, _children)
    {
        if (item.Id()==child->Id())
        {
            return index;
        }
        index++;
    }
    return -1;
}

} // namespace smcp
