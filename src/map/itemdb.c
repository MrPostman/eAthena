// Copyright (c) Athena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "../common/nullpo.h"
#include "../common/malloc.h"
#include "../common/showmsg.h"
#include "../common/strlib.h"
#include "../common/random.h"
#include "../common/conf.h"
#include "itemdb.h"
#include "map.h"
#include "battle.h" // struct battle_config
#include "script.h" // item script processing
#include "pc.h"     // W_MUSICAL, W_WHIP

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 32k array entries (the rest goes to the db)
#define MAX_ITEMDB 0x8000



static struct item_data* itemdb_array[MAX_ITEMDB];
static DBMap*            itemdb_other;// int nameid -> struct item_data*

static struct item_group itemgroup_db[MAX_ITEMGROUP];

struct item_data dummy_item; //This is the default dummy item used for non-existant items. [Skotlex]


struct item_package *itemdb_packages;
unsigned short itemdb_package_count;

/*==========================================
 * 名前で検索用
 *------------------------------------------*/
// name = item alias, so we should find items aliases first. if not found then look for "jname" (full name)
static int itemdb_searchname_sub(DBKey key,void *data,va_list ap)
{
	struct item_data *item=(struct item_data *)data,**dst,**dst2;
	char *str;
	str=va_arg(ap,char *);
	dst=va_arg(ap,struct item_data **);
	dst2=va_arg(ap,struct item_data **);
	if(item == &dummy_item) return 0;

	//Absolute priority to Aegis code name.
	if (*dst != NULL) return 0;
	if( strcmpi(item->name,str)==0 )
		*dst=item;

	//Second priority to Client displayed name.
	if (*dst2 != NULL) return 0;
	if( strcmpi(item->jname,str)==0 )
		*dst2=item;
	return 0;
}

/*==========================================
 * 名前で検索
 *------------------------------------------*/
struct item_data* itemdb_searchname(const char *str)
{
	struct item_data* item;
	struct item_data* item2=NULL;
	int i;

	for( i = 0; i < ARRAYLENGTH(itemdb_array); ++i )
	{
		item = itemdb_array[i];
		if( item == NULL )
			continue;

		// Absolute priority to Aegis code name.
		if( strcasecmp(item->name,str) == 0 )
			return item;

		//Second priority to Client displayed name.
		if( strcasecmp(item->jname,str) == 0 )
			item2 = item;
	}

	item = NULL;
	itemdb_other->foreach(itemdb_other,itemdb_searchname_sub,str,&item,&item2);
	return item?item:item2;
}

static int itemdb_searchname_array_sub(DBKey key,void * data,va_list ap)
{
	struct item_data *item=(struct item_data *)data;
	char *str;
	str=va_arg(ap,char *);
	if (item == &dummy_item)
		return 1; //Invalid item.
	if(stristr(item->jname,str))
		return 0;
	if(stristr(item->name,str))
		return 0;
	return strcmpi(item->jname,str);
}

/*==========================================
 * Founds up to N matches. Returns number of matches [Skotlex]
 *------------------------------------------*/
int itemdb_searchname_array(struct item_data** data, int size, const char *str)
{
	struct item_data* item;
	int i;
	int count=0;

	// Search in the array
	for( i = 0; i < ARRAYLENGTH(itemdb_array); ++i )
	{
		item = itemdb_array[i];
		if( item == NULL )
			continue;

		if( stristr(item->jname,str) || stristr(item->name,str) )
		{
			if( count < size )
				data[count] = item;
			++count;
		}
	}

	// search in the db
	if( count >= size )
	{
		data = NULL;
		size = 0;
	}
	else
	{
		data -= count;
		size -= count;
	}
	return count + itemdb_other->getall(itemdb_other,(void**)data,size,itemdb_searchname_array_sub,str);
}


/*==========================================
 * 箱系アイテム検索
 *------------------------------------------*/
int itemdb_searchrandomid(int group)
{
	if(group<1 || group>=MAX_ITEMGROUP) {
		ShowError("itemdb_searchrandomid: Invalid group id %d\n", group);
		return UNKNOWN_ITEM_ID;
	}
	if (itemgroup_db[group].qty)
		return itemgroup_db[group].nameid[rand()%itemgroup_db[group].qty];
	
	ShowError("itemdb_searchrandomid: No item entries for group id %d\n", group);
	return UNKNOWN_ITEM_ID;
}

/*==========================================
 * Calculates total item-group related bonuses for the given item
 *------------------------------------------*/
int itemdb_group_bonus(struct map_session_data* sd, int itemid)
{
	int bonus = 0, i, j;
	for (i=0; i < MAX_ITEMGROUP; i++) {
		if (!sd->itemgrouphealrate[i])
			continue;
		ARR_FIND( 0, itemgroup_db[i].qty, j, itemgroup_db[i].nameid[j] == itemid );
		if( j < itemgroup_db[i].qty )
			bonus += sd->itemgrouphealrate[i];
	}
	return bonus;
}

/// Searches for the item_data.
/// Returns the item_data or NULL if it does not exist.
struct item_data* itemdb_exists(int nameid)
{
	struct item_data* item;

	if( nameid >= 0 && nameid < ARRAYLENGTH(itemdb_array) )
		return itemdb_array[nameid];
	item = (struct item_data*)idb_get(itemdb_other,nameid);
	if( item == &dummy_item )
		return NULL;// dummy data, doesn't exist
	return item;
}

/// Returns human readable name for given item type.
/// @param type Type id to retrieve name for ( IT_* ).
const char* itemdb_typename(int type)
{
	switch(type)
	{
		case IT_HEALING:        return "Potion/Food";
		case IT_USABLE:         return "Usable";
		case IT_ETC:            return "Etc.";
		case IT_WEAPON:         return "Weapon";
		case IT_ARMOR:          return "Armor";
		case IT_CARD:           return "Card";
		case IT_PETEGG:         return "Pet Egg";
		case IT_PETARMOR:       return "Pet Accessory";
		case IT_AMMO:           return "Arrow/Ammunition";
		case IT_DELAYCONSUME:   return "Delay-Consume Usable";
		case IT_CASH:           return "Cash Usable";
	}
	return "Unknown Type";
}

/*==========================================
 * Converts the jobid from the format in itemdb 
 * to the format used by the map server. [Skotlex]
 *------------------------------------------*/
static void itemdb_jobid2mapid(unsigned int *bclass, unsigned int jobmask)
{
	int i;
	bclass[0]= bclass[1]= bclass[2]= 0;
	//Base classes
	if (jobmask & 1<<JOB_NOVICE)
	{	//Both Novice/Super-Novice are counted with the same ID
		bclass[0] |= 1<<MAPID_NOVICE;
		bclass[1] |= 1<<MAPID_NOVICE;
	}
	for (i = JOB_NOVICE+1; i <= JOB_THIEF; i++)
	{
		if (jobmask & 1<<i)
			bclass[0] |= 1<<(MAPID_NOVICE+i);
	}
	//2-1 classes
	if (jobmask & 1<<JOB_KNIGHT)
		bclass[1] |= 1<<MAPID_SWORDMAN;
	if (jobmask & 1<<JOB_PRIEST)
		bclass[1] |= 1<<MAPID_ACOLYTE;
	if (jobmask & 1<<JOB_WIZARD)
		bclass[1] |= 1<<MAPID_MAGE;
	if (jobmask & 1<<JOB_BLACKSMITH)
		bclass[1] |= 1<<MAPID_MERCHANT;
	if (jobmask & 1<<JOB_HUNTER)
		bclass[1] |= 1<<MAPID_ARCHER;
	if (jobmask & 1<<JOB_ASSASSIN)
		bclass[1] |= 1<<MAPID_THIEF;
	//2-2 classes
	if (jobmask & 1<<JOB_CRUSADER)
		bclass[2] |= 1<<MAPID_SWORDMAN;
	if (jobmask & 1<<JOB_MONK)
		bclass[2] |= 1<<MAPID_ACOLYTE;
	if (jobmask & 1<<JOB_SAGE)
		bclass[2] |= 1<<MAPID_MAGE;
	if (jobmask & 1<<JOB_ALCHEMIST)
		bclass[2] |= 1<<MAPID_MERCHANT;
	if (jobmask & 1<<JOB_BARD)
		bclass[2] |= 1<<MAPID_ARCHER;
//	Bard/Dancer share the same slot now.
//	if (jobmask & 1<<JOB_DANCER)
//		bclass[2] |= 1<<MAPID_ARCHER;
	if (jobmask & 1<<JOB_ROGUE)
		bclass[2] |= 1<<MAPID_THIEF;
	//Special classes that don't fit above.
	if (jobmask & 1<<21) //Taekwon boy
		bclass[0] |= 1<<MAPID_TAEKWON;
	if (jobmask & 1<<22) //Star Gladiator
		bclass[1] |= 1<<MAPID_TAEKWON;
	if (jobmask & 1<<23) //Soul Linker
		bclass[2] |= 1<<MAPID_TAEKWON;
	if (jobmask & 1<<JOB_GUNSLINGER)
		bclass[0] |= 1<<MAPID_GUNSLINGER;
	if (jobmask & 1<<JOB_NINJA)
		bclass[0] |= 1<<MAPID_NINJA;
}

static void create_dummy_data(void)
{
	memset(&dummy_item, 0, sizeof(struct item_data));
	dummy_item.nameid=500;
	dummy_item.weight=1;
	dummy_item.value_sell=1;
	dummy_item.type=IT_ETC; //Etc item
	safestrncpy(dummy_item.name,"UNKNOWN_ITEM",sizeof(dummy_item.name));
	safestrncpy(dummy_item.jname,"UNKNOWN_ITEM",sizeof(dummy_item.jname));
	dummy_item.view_id=UNKNOWN_ITEM_ID;
}

static struct item_data* create_item_data(int nameid)
{
	struct item_data *id;
	CREATE(id, struct item_data, 1);
	id->nameid = nameid;
	id->weight = 1;
	id->type = IT_ETC;
	return id;
}

/*==========================================
 * Loads (and creates if not found) an item from the db.
 *------------------------------------------*/
struct item_data* itemdb_load(int nameid)
{
	struct item_data *id;

	if( nameid >= 0 && nameid < ARRAYLENGTH(itemdb_array) )
	{
		id = itemdb_array[nameid];
		if( id == NULL || id == &dummy_item )
			id = itemdb_array[nameid] = create_item_data(nameid);
		return id;
	}

	id = (struct item_data*)idb_get(itemdb_other, nameid);
	if( id == NULL || id == &dummy_item )
	{
		id = create_item_data(nameid);
		idb_put(itemdb_other, nameid, id);
	}
	return id;
}

/*==========================================
 * Loads an item from the db. If not found, it will return the dummy item.
 *------------------------------------------*/
struct item_data* itemdb_search(int nameid)
{
	struct item_data* id;
	if( nameid >= 0 && nameid < ARRAYLENGTH(itemdb_array) )
		id = itemdb_array[nameid];
	else
		id = (struct item_data*)idb_get(itemdb_other, nameid);

	if( id == NULL )
	{
		ShowWarning("itemdb_search: Item ID %d does not exists in the item_db. Using dummy data.\n", nameid);
		id = &dummy_item;
		dummy_item.nameid = nameid;
	}
	return id;
}

/*==========================================
 * Returns if given item is a player-equippable piece.
 *------------------------------------------*/
int itemdb_isequip(int nameid)
{
	int type=itemdb_type(nameid);
	switch (type) {
		case IT_WEAPON:
		case IT_ARMOR:
		case IT_AMMO:
			return 1;
		default:
			return 0;
	}
}

/*==========================================
 * Alternate version of itemdb_isequip
 *------------------------------------------*/
int itemdb_isequip2(struct item_data *data)
{ 
	nullpo_ret(data);
	switch(data->type) {
		case IT_WEAPON:
		case IT_ARMOR:
		case IT_AMMO:
			return 1;
		default:
			return 0;
	}
}

/*==========================================
 * Returns if given item's type is stackable.
 *------------------------------------------*/
int itemdb_isstackable(int nameid)
{
  int type=itemdb_type(nameid);
  switch(type) {
	  case IT_WEAPON:
	  case IT_ARMOR:
	  case IT_PETEGG:
	  case IT_PETARMOR:
		  return 0;
	  default:
		  return 1;
  }
}

/*==========================================
 * Alternate version of itemdb_isstackable
 *------------------------------------------*/
int itemdb_isstackable2(struct item_data *data)
{
  nullpo_ret(data);
  switch(data->type) {
	  case IT_WEAPON:
	  case IT_ARMOR:
	  case IT_PETEGG:
	  case IT_PETARMOR:
		  return 0;
	  default:
		  return 1;
  }
}


/*==========================================
 * Trade Restriction functions [Skotlex]
 *------------------------------------------*/
int itemdb_isdropable_sub(struct item_data *item, int gmlv, int unused)
{
	return (item && (!(item->flag.trade_restriction&1) || gmlv >= item->gm_lv_trade_override));
}

int itemdb_cantrade_sub(struct item_data* item, int gmlv, int gmlv2)
{
	return (item && (!(item->flag.trade_restriction&2) || gmlv >= item->gm_lv_trade_override || gmlv2 >= item->gm_lv_trade_override));
}

int itemdb_canpartnertrade_sub(struct item_data* item, int gmlv, int gmlv2)
{
	return (item && (item->flag.trade_restriction&4 || gmlv >= item->gm_lv_trade_override || gmlv2 >= item->gm_lv_trade_override));
}

int itemdb_cansell_sub(struct item_data* item, int gmlv, int unused)
{
	return (item && (!(item->flag.trade_restriction&8) || gmlv >= item->gm_lv_trade_override));
}

int itemdb_cancartstore_sub(struct item_data* item, int gmlv, int unused)
{	
	return (item && (!(item->flag.trade_restriction&16) || gmlv >= item->gm_lv_trade_override));
}

int itemdb_canstore_sub(struct item_data* item, int gmlv, int unused)
{	
	return (item && (!(item->flag.trade_restriction&32) || gmlv >= item->gm_lv_trade_override));
}

int itemdb_canguildstore_sub(struct item_data* item, int gmlv, int unused)
{	
	return (item && (!(item->flag.trade_restriction&64) || gmlv >= item->gm_lv_trade_override));
}

int itemdb_isrestricted(struct item* item, int gmlv, int gmlv2, int (*func)(struct item_data*, int, int))
{
	struct item_data* item_data = itemdb_search(item->nameid);
	int i;

	if (!func(item_data, gmlv, gmlv2))
		return 0;
	
	if(item_data->slot == 0 || itemdb_isspecial(item->card[0]))
		return 1;
	
	for(i = 0; i < item_data->slot; i++) {
		if (!item->card[i]) continue;
		if (!func(itemdb_search(item->card[i]), gmlv, gmlv2))
			return 0;
	}
	return 1;
}

/*==========================================
 *	Specifies if item-type should drop unidentified.
 *------------------------------------------*/
int itemdb_isidentified(int nameid)
{
	int type=itemdb_type(nameid);
	switch (type) {
		case IT_WEAPON:
		case IT_ARMOR:
		case IT_PETARMOR:
			return 0;
		default:
			return 1;
	}
}

/*==========================================
 * アイテム使用可能フラグのオーバーライド
 *------------------------------------------*/
static bool itemdb_read_itemavail(char* str[], int columns, int current)
{// <nameid>,<sprite>
	int nameid, sprite;
	struct item_data *id;

	nameid = atoi(str[0]);

	if( ( id = itemdb_exists(nameid) ) == NULL )
	{
		ShowWarning("itemdb_read_itemavail: Invalid item id %d.\n", nameid);
		return false;
	}

	sprite = atoi(str[1]);

	if( sprite > 0 )
	{
		id->flag.available = 1;
		id->view_id = sprite;
	}
	else
	{
		id->flag.available = 0;
	}

	return true;
}

/*==========================================
 * read item group data
 *------------------------------------------*/
static void itemdb_read_itemgroup_sub(const char* filename)
{
	FILE *fp;
	char line[1024];
	int ln=0;
	int groupid,j,k,nameid;
	char *str[3],*p;
	char w1[1024], w2[1024];
	
	if( (fp=fopen(filename,"r"))==NULL ){
		ShowError("can't read %s\n", filename);
		return;
	}

	while(fgets(line, sizeof(line), fp))
	{
		ln++;
		if(line[0]=='/' && line[1]=='/')
			continue;
		if(strstr(line,"import")) {
			if (sscanf(line, "%[^:]: %[^\r\n]", w1, w2) == 2 &&
				strcmpi(w1, "import") == 0) {
				itemdb_read_itemgroup_sub(w2);
				continue;
			}
		}
		memset(str,0,sizeof(str));
		for(j=0,p=line;j<3 && p;j++){
			str[j]=p;
			p=strchr(p,',');
			if(p) *p++=0;
		}
		if(str[0]==NULL)
			continue;
		if (j<3) {
			if (j>1) //Or else it barks on blank lines...
				ShowWarning("itemdb_read_itemgroup: Insufficient fields for entry at %s:%d\n", filename, ln);
			continue;
		}
		groupid = atoi(str[0]);
		if (groupid < 0 || groupid >= MAX_ITEMGROUP) {
			ShowWarning("itemdb_read_itemgroup: Invalid group %d in %s:%d\n", groupid, filename, ln);
			continue;
		}
		nameid = atoi(str[1]);
		if (!itemdb_exists(nameid)) {
			ShowWarning("itemdb_read_itemgroup: Non-existant item %d in %s:%d\n", nameid, filename, ln);
			continue;
		}
		k = atoi(str[2]);
		if (itemgroup_db[groupid].qty+k >= MAX_RANDITEM) {
			ShowWarning("itemdb_read_itemgroup: Group %d is full (%d entries) in %s:%d\n", groupid, MAX_RANDITEM, filename, ln);
			continue;
		}
		for(j=0;j<k;j++)
			itemgroup_db[groupid].nameid[itemgroup_db[groupid].qty++] = nameid;
	}
	fclose(fp);
	return;
}

static void itemdb_read_itemgroup(void)
{
	char path[256];
	snprintf(path, 255, "%s/item_group_db.txt", db_path);

	memset(&itemgroup_db, 0, sizeof(itemgroup_db));
	itemdb_read_itemgroup_sub(path);
	ShowStatus("Done reading '"CL_WHITE"%s"CL_RESET"'.\n", "item_group_db.txt");
	return;
}

/*==========================================
 * 装備制限ファイル読み出し
 *------------------------------------------*/
static bool itemdb_read_noequip(char* str[], int columns, int current)
{// <nameid>,<mode>
	int nameid;
	struct item_data *id;

	nameid = atoi(str[0]);

	if( ( id = itemdb_exists(nameid) ) == NULL )
	{
		ShowWarning("itemdb_read_noequip: Invalid item id %d.\n", nameid);
		return false;
	}

	id->flag.no_equip |= atoi(str[1]);

	return true;
}

/*==========================================
 * Reads item trade restrictions [Skotlex]
 *------------------------------------------*/
static bool itemdb_read_itemtrade(char* str[], int columns, int current)
{// <nameid>,<mask>,<gm level>
	int nameid, flag, gmlv;
	struct item_data *id;

	nameid = atoi(str[0]);

	if( ( id = itemdb_exists(nameid) ) == NULL )
	{
		//ShowWarning("itemdb_read_itemtrade: Invalid item id %d.\n", nameid);
		//return false;
		// FIXME: item_trade.txt contains items, which are commented in item database.
		return true;
	}

	flag = atoi(str[1]);
	gmlv = atoi(str[2]);

	if( flag < 0 || flag >= 128 )
	{//Check range
		ShowWarning("itemdb_read_itemtrade: Invalid trading mask %d for item id %d.\n", flag, nameid);
		return false;
	}

	if( gmlv < 1 )
	{
		ShowWarning("itemdb_read_itemtrade: Invalid override GM level %d for item id %d.\n", gmlv, nameid);
		return false;
	}

	id->flag.trade_restriction = flag;
	id->gm_lv_trade_override = gmlv;

	return true;
}

/*==========================================
 * Reads item delay amounts [Paradox924X]
 *------------------------------------------*/
static bool itemdb_read_itemdelay(char* str[], int columns, int current)
{// <nameid>,<delay>
	int nameid, delay;
	struct item_data *id;

	nameid = atoi(str[0]);

	if( ( id = itemdb_exists(nameid) ) == NULL )
	{
		ShowWarning("itemdb_read_itemdelay: Invalid item id %d.\n", nameid);
		return false;
	}

	delay = atoi(str[1]);

	if( delay < 0 )
	{
		ShowWarning("itemdb_read_itemdelay: Invalid delay %d for item id %d.\n", id->delay, nameid);
		return false;
	}

	id->delay = delay;

	return true;
}


/// Reads items allowed to be sold in buying stores
static bool itemdb_read_buyingstore(char* fields[], int columns, int current)
{// <nameid>
	int nameid;
	struct item_data* id;

	nameid = atoi(fields[0]);

	if( ( id = itemdb_exists(nameid) ) == NULL )
	{
		ShowWarning("itemdb_read_buyingstore: Invalid item id %d.\n", nameid);
		return false;
	}

	if( !itemdb_isstackable2(id) )
	{
		ShowWarning("itemdb_read_buyingstore: Non-stackable item id %d cannot be enabled for buying store.\n", nameid);
		return false;
	}

	id->flag.buyingstore = true;

	return true;
}


/*======================================
 * Applies gender restrictions according to settings. [Skotlex]
 *======================================*/
static int itemdb_gendercheck(struct item_data *id)
{
	if (id->nameid == WEDDING_RING_M) //Grom Ring
		return 1;
	if (id->nameid == WEDDING_RING_F) //Bride Ring
		return 0;
	if (id->look == W_MUSICAL && id->type == IT_WEAPON) //Musical instruments are always male-only
		return 1;
	if (id->look == W_WHIP && id->type == IT_WEAPON) //Whips are always female-only
		return 0;

	return (battle_config.ignore_items_gender) ? 2 : id->sex;
}

/*==========================================
 * processes one itemdb entry
 *------------------------------------------*/
static bool itemdb_parse_dbrow(char** str, const char* source, int line, int scriptopt)
{
	/*
		+----+--------------+---------------+------+-----------+------------+--------+--------+---------+-------+-------+------------+-------------+---------------+-----------------+--------------+-------------+------------+------+--------+--------------+----------------+
		| 00 |      01      |       02      |  03  |     04    |     05     |   06   |   07   |    08   |   09  |   10  |     11     |      12     |       13      |        14       |      15      |      16     |     17     |  18  |   19   |      20      |        21      |
		+----+--------------+---------------+------+-----------+------------+--------+--------+---------+-------+-------+------------+-------------+---------------+-----------------+--------------+-------------+------------+------+--------+--------------+----------------+
		| id | name_english | name_japanese | type | price_buy | price_sell | weight | attack | defence | range | slots | equip_jobs | equip_upper | equip_genders | equip_locations | weapon_level | equip_level | refineable | view | script | equip_script | unequip_script |
		+----+--------------+---------------+------+-----------+------------+--------+--------+---------+-------+-------+------------+-------------+---------------+-----------------+--------------+-------------+------------+------+--------+--------------+----------------+
	*/
	int nameid;
	struct item_data* id;
	
	nameid = atoi(str[0]);
	if( nameid <= 0 )
	{
		ShowWarning("itemdb_parse_dbrow: Invalid id %d in line %d of \"%s\", skipping.\n", nameid, line, source);
		return false;
	}

	//ID,Name,Jname,Type,Price,Sell,Weight,ATK,DEF,Range,Slot,Job,Job Upper,Gender,Loc,wLV,eLV,refineable,View
	id = itemdb_load(nameid);
	safestrncpy(id->name, str[1], sizeof(id->name));
	safestrncpy(id->jname, str[2], sizeof(id->jname));

	id->type = atoi(str[3]);

	if( id->type < 0 || id->type == IT_UNKNOWN || id->type == IT_UNKNOWN2 || ( id->type > IT_DELAYCONSUME && id->type < IT_CASH ) || id->type >= IT_MAX )
	{// catch invalid item types
		ShowWarning("itemdb_parse_dbrow: Invalid item type %d for item %d. IT_ETC will be used.\n", id->type, nameid);
		id->type = IT_ETC;
	}

	if (id->type == IT_DELAYCONSUME)
	{	//Items that are consumed only after target confirmation
		id->type = IT_USABLE;
		id->flag.delay_consume = 1;
	} else //In case of an itemdb reload and the item type changed.
		id->flag.delay_consume = 0;

	//When a particular price is not given, we should base it off the other one
	//(it is important to make a distinction between 'no price' and 0z)
	if ( str[4][0] )
		id->value_buy = atoi(str[4]);
	else
		id->value_buy = atoi(str[5]) * 2;

	if ( str[5][0] )
		id->value_sell = atoi(str[5]);
	else
		id->value_sell = id->value_buy / 2;
	/* 
	if ( !str[4][0] && !str[5][0])
	{  
		ShowWarning("itemdb_parse_dbrow: No buying/selling price defined for item %d (%s), using 20/10z\n",       nameid, id->jname);
		id->value_buy = 20;
		id->value_sell = 10;
	} else
	*/
	if (id->value_buy/124. < id->value_sell/75.)
		ShowWarning("itemdb_parse_dbrow: Buying/Selling [%d/%d] price of item %d (%s) allows Zeny making exploit  through buying/selling at discounted/overcharged prices!\n",
			id->value_buy, id->value_sell, nameid, id->jname);

	id->weight = atoi(str[6]);
	id->atk = atoi(str[7]);
	id->def = atoi(str[8]);
	id->range = atoi(str[9]);
	id->slot = atoi(str[10]);

	if (id->slot > MAX_SLOTS)
	{
		ShowWarning("itemdb_parse_dbrow: Item %d (%s) specifies %d slots, but the server only supports up to %d. Using %d slots.\n", nameid, id->jname, id->slot, MAX_SLOTS, MAX_SLOTS);
		id->slot = MAX_SLOTS;
	}

	itemdb_jobid2mapid(id->class_base, (unsigned int)strtoul(str[11],NULL,0));
	id->class_upper = atoi(str[12]);
	id->sex	= atoi(str[13]);
	id->equip = atoi(str[14]);

	if (!id->equip && itemdb_isequip2(id))
	{
		ShowWarning("Item %d (%s) is an equipment with no equip-field! Making it an etc item.\n", nameid, id->jname);
		id->type = IT_ETC;
	}

	id->wlv = atoi(str[15]);
	id->elv = atoi(str[16]);
	id->flag.no_refine = atoi(str[17]) ? 0 : 1; //FIXME: verify this
	id->look = atoi(str[18]);

	id->flag.available = 1;
	id->view_id = 0;
	id->sex = itemdb_gendercheck(id); //Apply gender filtering.

	if (id->script)
	{
		script_free_code(id->script);
		id->script = NULL;
	}
	if (id->equip_script)
	{
		script_free_code(id->equip_script);
		id->equip_script = NULL;
	}
	if (id->unequip_script)
	{
		script_free_code(id->unequip_script);
		id->unequip_script = NULL;
	}

	if (*str[19])
		id->script = parse_script(str[19], source, line, scriptopt);
	if (*str[20])
		id->equip_script = parse_script(str[20], source, line, scriptopt);
	if (*str[21])
		id->unequip_script = parse_script(str[21], source, line, scriptopt);

	return true;
}

/*==========================================
 * アイテムデータベースの読み込み
 *------------------------------------------*/
static int itemdb_readdb(void)
{
	const char* filename[] = { "item_db.txt", "item_db2.txt" };
	int fi;

	for( fi = 0; fi < ARRAYLENGTH(filename); ++fi )
	{
		uint32 lines = 0, count = 0;
		char line[1024];

		char path[256];
		FILE* fp;

		sprintf(path, "%s/%s", db_path, filename[fi]);
		fp = fopen(path, "r");
		if( fp == NULL )
		{
			ShowWarning("itemdb_readdb: File not found \"%s\", skipping.\n", path);
			continue;
		}

		// process rows one by one
		while(fgets(line, sizeof(line), fp))
		{
			char *str[32], *p;
			int i;

			lines++;
			if(line[0] == '/' && line[1] == '/')
				continue;
			memset(str, 0, sizeof(str));

			p = line;
			while( ISSPACE(*p) )
				++p;
			if( *p == '\0' )
				continue;// empty line
			for( i = 0; i < 19; ++i )
			{
				str[i] = p;
				p = strchr(p,',');
				if( p == NULL )
					break;// comma not found
				*p = '\0';
				++p;
			}

			if( p == NULL )
			{
				ShowError("itemdb_readdb: Insufficient columns in line %d of \"%s\" (item with id %d), skipping.\n", lines, path, atoi(str[0]));
				continue;
			}

			// Script
			if( *p != '{' )
			{
				ShowError("itemdb_readdb: Invalid format (Script column) in line %d of \"%s\" (item with id %d), skipping.\n", lines, path, atoi(str[0]));
				continue;
			}
			str[19] = p;
			p = strstr(p+1,"},");
			if( p == NULL )
			{
				ShowError("itemdb_readdb: Invalid format (Script column) in line %d of \"%s\" (item with id %d), skipping.\n", lines, path, atoi(str[0]));
				continue;
			}
			p[1] = '\0';
			p += 2;

			// OnEquip_Script
			if( *p != '{' )
			{
				ShowError("itemdb_readdb: Invalid format (OnEquip_Script column) in line %d of \"%s\" (item with id %d), skipping.\n", lines, path, atoi(str[0]));
				continue;
			}
			str[20] = p;
			p = strstr(p+1,"},");
			if( p == NULL )
			{
				ShowError("itemdb_readdb: Invalid format (OnEquip_Script column) in line %d of \"%s\" (item with id %d), skipping.\n", lines, path, atoi(str[0]));
				continue;
			}
			p[1] = '\0';
			p += 2;

			// OnUnequip_Script (last column)
			if( *p != '{' )
			{
				ShowError("itemdb_readdb: Invalid format (OnUnequip_Script column) in line %d of \"%s\" (item with id %d), skipping.\n", lines, path, atoi(str[0]));
				continue;
			}
			str[21] = p;


			if (!itemdb_parse_dbrow(str, path, lines, 0))
				continue;

			count++;
		}

		fclose(fp);

		ShowStatus("Done reading '"CL_WHITE"%lu"CL_RESET"' entries in '"CL_WHITE"%s"CL_RESET"'.\n", count, filename[fi]);
	}

	return 0;
}

#ifndef TXT_ONLY
/*======================================
 * item_db table reading
 *======================================*/
static int itemdb_read_sqldb(void)
{
	const char* item_db_name[] = { item_db_db, item_db2_db };
	int fi;
	
	for( fi = 0; fi < ARRAYLENGTH(item_db_name); ++fi )
	{
		uint32 lines = 0, count = 0;

		// retrieve all rows from the item database
		if( SQL_ERROR == Sql_Query(mmysql_handle, "SELECT * FROM `%s`", item_db_name[fi]) )
		{
			Sql_ShowDebug(mmysql_handle);
			continue;
		}

		// process rows one by one
		while( SQL_SUCCESS == Sql_NextRow(mmysql_handle) )
		{// wrap the result into a TXT-compatible format
			char* str[22];
			char* dummy = "";
			int i;
			++lines;
			for( i = 0; i < 22; ++i )
			{
				Sql_GetData(mmysql_handle, i, &str[i], NULL);
				if( str[i] == NULL ) str[i] = dummy; // get rid of NULL columns
			}

			if (!itemdb_parse_dbrow(str, item_db_name[fi], lines, SCRIPT_IGNORE_EXTERNAL_BRACKETS))
				continue;
			++count;
		}

		// free the query result
		Sql_FreeResult(mmysql_handle);

		ShowStatus("Done reading '"CL_WHITE"%lu"CL_RESET"' entries in '"CL_WHITE"%s"CL_RESET"'.\n", count, item_db_name[fi]);
	}

	return 0;
}
#endif /* not TXT_ONLY */

/* [Ind/Hercules] */
void itemdb_package_item(struct map_session_data *sd, struct item_package *package) {
	int i = 0, get_count, j, flag;

	nullpo_retv(sd);
	nullpo_retv(package);

	for (i = 0; i < package->must_qty; i++) {
		struct item it;
		memset(&it, 0, sizeof(it));

		it.nameid = package->must_items[i].id;
		it.identify = 1;

		if (package->must_items[i].hours) {
			it.expire_time = (unsigned int)(time(NULL) + ((package->must_items[i].hours * 60) * 60));
		}

		if (package->must_items[i].named) {
			it.card[0] = CARD0_FORGE;
			it.card[1] = 0;
			it.card[2] = GetWord(sd->status.char_id, 0);
			it.card[3] = GetWord(sd->status.char_id, 1);
		}

		if (package->must_items[i].announce)
			;//clif_package_announce(sd, package->must_items[i].id, package->id);

		get_count = itemdb_isstackable(package->must_items[i].id) ? package->must_items[i].qty : 1;
		it.amount = get_count == 1 ? 1 : get_count;

		for (j = 0; j < package->must_items[i].qty; j += get_count) {
			if ((flag = pc_additem(sd, &it, get_count)))
				clif_additem(sd, 0, 0, flag);
		}
	}


	if (package->random_qty) {
		for (i = 0; i < package->random_qty; i++) {
			struct item_package_rand_entry *entry;

			entry = &package->random_groups[i].random_list[rnd() % package->random_groups[i].random_qty];

			while (1) {
				if (rnd() % 10000 >= entry->rate) {
					entry = entry->next;
					continue;
				}
				else {
					struct item it;
					memset(&it, 0, sizeof(it));

					it.nameid = entry->id;
					it.identify = 1;

					if (entry->hours) {
						it.expire_time = (unsigned int)(time(NULL) + ((entry->hours * 60) * 60));
					}

					if (entry->named) {
						it.card[0] = CARD0_FORGE;
						it.card[1] = 0;
						it.card[2] = GetWord(sd->status.char_id, 0);
						it.card[3] = GetWord(sd->status.char_id, 1);
					}

					if (entry->announce)
						;// clif_package_announce(sd, entry->id, package->id);

					get_count = itemdb_isstackable(entry->id) ? entry->qty : 1;

					it.amount = get_count == 1 ? 1 : get_count;

					for (j = 0; j < entry->qty; j += get_count) {
						if ((flag = pc_additem(sd, &it, get_count)))
							clif_additem(sd, 0, 0, flag);
					}
					break;
				}
			}
		}
	}
}

/*======================================
* [Ind/Hercules]
*======================================*/
static void itemdb_read_packages(void) {
	const char *config_filename = "db/item_packages.conf";
	config_t item_packages_conf;
	config_setting_t *itg = NULL, *it = NULL, *t = NULL;
	unsigned int *must = NULL, *random = NULL, *rgroup = NULL, **rgroups = NULL;
	const char *itname;
	struct item_package_rand_entry **prev = NULL;
	int i = 0, c = 0, count = 0, highest_gcount = 0;

	if (config_load(&item_packages_conf, config_filename)) {
		ShowError("can't read %s\n", config_filename);
		return;
	}

	must = aMalloc(config_setting_length(item_packages_conf.root) * sizeof(unsigned int));
	random = aMalloc(config_setting_length(item_packages_conf.root) * sizeof(unsigned int));
	rgroup = aMalloc(config_setting_length(item_packages_conf.root) * sizeof(unsigned int));
	rgroups = aMalloc(config_setting_length(item_packages_conf.root) * sizeof(unsigned int *));

	for (i = 0; i < config_setting_length(item_packages_conf.root); i++) {
		must[i] = 0;
		random[i] = 0;
		rgroup[i] = 0;
		rgroups[i] = NULL;
	}

	/* validate tree, drop poisonous fruits! */
	i = 0;
	while ((itg = config_setting_get_elem(item_packages_conf.root, i++))) {
		const char *name = config_setting_name(itg);

		if (!itemdb_searchname(name)) {
			ShowWarning("itemdb_read_packages: unknown package item '%s', skipping..\n", name);
			config_setting_remove(item_packages_conf.root, name);
			--i;
			continue;
		}

		c = 0;
		while ((it = config_setting_get_elem(itg, c++))) {
			int rval = 0;

			if (!(t = config_setting_get_member(it, "Random")) || (rval = config_setting_get_int(t)) < 0) {
				ShowWarning("itemdb_read_packages: invalid 'Random' value (%d) for item '%s' in package '%s', defaulting to must!\n", rval, config_setting_name(it), name);
				config_setting_remove(it, config_setting_name(it));
				--c;
				continue;
			}

			if (rval == 0)
				must[i - 1] += 1;
			else {
				random[i - 1] += 1;
				if (rval > rgroup[i - 1])
					rgroup[i - 1] = rval;
				if (rval > highest_gcount)
					highest_gcount = rval;
			}
		}
	}

	CREATE(prev, struct item_package_rand_entry *, highest_gcount);
	for (i = 0; i < highest_gcount; i++) {
		prev[i] = NULL;
	}

	for (i = 0; i < config_setting_length(item_packages_conf.root); i++) {
		rgroups[i] = aMalloc(rgroup[i] * sizeof(unsigned int));
		for (c = 0; c < rgroup[i]; c++) {
			rgroups[i][c] = 0;
		}
	}

	/* grab the known sizes */
	i = 0;
	while ((itg = config_setting_get_elem(item_packages_conf.root, i++))) {
		c = 0;
		while ((it = config_setting_get_elem(itg, c++))) {
			int rval = 0;
			if ((t = config_setting_get_member(it, "Random")) != NULL && (rval = config_setting_get_int(t)) > 0) {
				rgroups[i - 1][rval - 1] += 1;
			}
		}
	}

	CREATE(itemdb_packages, struct item_package, config_setting_length(item_packages_conf.root));
	itemdb_package_count = (unsigned short)config_setting_length(item_packages_conf.root);

	/* write */
	i = 0;
	while ((itg = config_setting_get_elem(item_packages_conf.root, i++))) {
		struct item_data *data = itemdb_searchname(config_setting_name(itg));
		int r = 0, m = 0;

		for (r = 0; r < highest_gcount; r++) {
			prev[r] = NULL;
		}

		data->package = &itemdb_packages[count];

		itemdb_packages[count].id = data->nameid;
		itemdb_packages[count].random_groups = NULL;
		itemdb_packages[count].must_items = NULL;
		itemdb_packages[count].random_qty = rgroup[i - 1];
		itemdb_packages[count].must_qty = must[i - 1];

		if (itemdb_packages[count].random_qty) {
			CREATE(itemdb_packages[count].random_groups, struct item_package_rand_group, itemdb_packages[count].random_qty);
			for (c = 0; c < itemdb_packages[count].random_qty; c++) {
				if (!rgroups[i - 1][c])
					ShowError("itemdb_read_packages: package '%s' missing 'Random' field %d! there must not be gaps!\n", config_setting_name(itg), c + 1);
				else
					CREATE(itemdb_packages[count].random_groups[c].random_list, struct item_package_rand_entry, rgroups[i - 1][c]);
				itemdb_packages[count].random_groups[c].random_qty = 0;
			}
		}

		if (itemdb_packages[count].must_qty)
			CREATE(itemdb_packages[count].must_items, struct item_package_must_entry, itemdb_packages[count].must_qty);

		c = 0;
		while ((it = config_setting_get_elem(itg, c++))) {
			int icount = 1, expire = 0, rate = 10000, gid = 0;
			bool announce = false, named = false, force_serial = false;

			itname = config_setting_name(it);

			if (itname[0] == 'I' && itname[1] == 'D' && strlen(itname) < 8) {
				if (!(data = itemdb_exists(atoi(itname + 2))))
					ShowWarning("itemdb_read_packages: unknown item ID '%d' in package '%s'!\n", atoi(itname + 2), config_setting_name(itg));
			}
			else if (!(data = itemdb_searchname(itname)))
				ShowWarning("itemdb_read_packages: unknown item '%s' in package '%s'!\n", itname, config_setting_name(itg));

			if ((t = config_setting_get_member(it, "Count")))
				icount = config_setting_get_int(t);

			if ((t = config_setting_get_member(it, "Expire")))
				expire = config_setting_get_int(t);

			if ((t = config_setting_get_member(it, "Rate"))) {
				if ((rate = (unsigned short)config_setting_get_int(t)) > 10000) {
					ShowWarning("itemdb_read_packages: invalid rate (%d) for item '%s' in package '%s'!\n", rate, itname, config_setting_name(itg));
					rate = 10000;
				}
			}

			if ((t = config_setting_get_member(it, "Announce")) && config_setting_get_bool(t))
				announce = true;

			if ((t = config_setting_get_member(it, "Named")) && config_setting_get_bool(t))
				named = true;

			if ((t = config_setting_get_member(it, "ForceSerial")) && config_setting_get_bool(t))
				force_serial = true;

			if (!(t = config_setting_get_member(it, "Random"))) {
				ShowWarning("itemdb_read_packages: missing 'Random' field for item '%s' in package '%s', defaulting to must!\n", itname, config_setting_name(itg));
				gid = 0;
			}
			else
				gid = config_setting_get_int(t);

			if (gid == 0) {
				itemdb_packages[count].must_items[m].id = data ? data->nameid : 0;
				itemdb_packages[count].must_items[m].qty = icount;
				itemdb_packages[count].must_items[m].hours = expire;
				itemdb_packages[count].must_items[m].announce = announce == true ? 1 : 0;
				itemdb_packages[count].must_items[m].named = named == true ? 1 : 0;
				itemdb_packages[count].must_items[m].force_serial = force_serial == true ? 1 : 0;
				m++;
			}
			else {
				int gidx = gid - 1;

				r = itemdb_packages[count].random_groups[gidx].random_qty;

				if (prev[gidx])
					prev[gidx]->next = &itemdb_packages[count].random_groups[gidx].random_list[r];

				itemdb_packages[count].random_groups[gidx].random_list[r].id = data ? data->nameid : 0;
				itemdb_packages[count].random_groups[gidx].random_list[r].qty = icount;
				if ((itemdb_packages[count].random_groups[gidx].random_list[r].rate = rate) == 10000) {
					ShowWarning("itemdb_read_packages: item '%s' in '%s' has 100%% drop rate!! set this item as 'Random: 0' or other items won't drop!!!\n", itname, config_setting_name(itg));
				}
				itemdb_packages[count].random_groups[gidx].random_list[r].hours = expire;
				itemdb_packages[count].random_groups[gidx].random_list[r].announce = announce == true ? 1 : 0;
				itemdb_packages[count].random_groups[gidx].random_list[r].named = named == true ? 1 : 0;
				itemdb_packages[count].random_groups[gidx].random_list[r].force_serial = force_serial == true ? 1 : 0;
				itemdb_packages[count].random_groups[gidx].random_qty += 1;

				prev[gidx] = &itemdb_packages[count].random_groups[gidx].random_list[r];
			}
		}

		for (r = 0; r < highest_gcount; r++) {
			if (prev[r])
				prev[r]->next = &itemdb_packages[count].random_groups[r].random_list[0];
		}

		for (r = 0; r < itemdb_packages[count].random_qty; r++) {
			if (itemdb_packages[count].random_groups[r].random_qty == 1) {
				//item packages don't stop looping until something comes out of them, so if you have only one item in it the drop is guaranteed.
				ShowWarning("itemdb_read_packages: in '%s' 'Random: %d' group has only 1 random option, drop rate will be 100%%!\n",
					itemdb_name(itemdb_packages[count].id), r + 1);
				itemdb_packages[count].random_groups[r].random_list[0].rate = 10000;
			}
		}
		count++;	
	}

	aFree(must);
	aFree(random);
	for (i = 0; i < config_setting_length(item_packages_conf.root); i++) {
		aFree(rgroups[i]);
	}
	aFree(rgroups);
	aFree(rgroup);
	aFree(prev);

	config_destroy(&item_packages_conf);
	ShowStatus("Done reading '"CL_WHITE"%d"CL_RESET"' entries in '"CL_WHITE"%s"CL_RESET"'.\n", count, config_filename);	
}

/*====================================
 * read all item-related databases
 *------------------------------------*/
static void itemdb_read(void)
{
#ifndef TXT_ONLY
	if (db_use_sqldbs)
		itemdb_read_sqldb();
	else
#endif
		itemdb_readdb();

	itemdb_read_itemgroup();
	sv_readdb(db_path, "item_avail.txt",   ',', 2, 2, -1,             &itemdb_read_itemavail);
	sv_readdb(db_path, "item_noequip.txt", ',', 2, 2, -1,             &itemdb_read_noequip);
	sv_readdb(db_path, "item_trade.txt",   ',', 3, 3, -1,             &itemdb_read_itemtrade);
	sv_readdb(db_path, "item_delay.txt",   ',', 2, 2, MAX_ITEMDELAYS, &itemdb_read_itemdelay);
	sv_readdb(db_path, "item_buyingstore.txt", ',', 1, 1, -1,         &itemdb_read_buyingstore);

	itemdb_read_packages();
}

/*==========================================
 * Initialize / Finalize
 *------------------------------------------*/

/// Destroys the item_data.
static void destroy_item_data(struct item_data* self, int free_self)
{
	if( self == NULL )
		return;
	// free scripts
	if( self->script )
		script_free_code(self->script);
	if( self->equip_script )
		script_free_code(self->equip_script);
	if( self->unequip_script )
		script_free_code(self->unequip_script);
#if defined(DEBUG)
	// trash item
	memset(self, 0xDD, sizeof(struct item_data));
#endif
	// free self
	if( free_self )
		aFree(self);
}
		
static int itemdb_final_sub(DBKey key,void *data,va_list ap)
{
	struct item_data *id = (struct item_data *)data;

	if( id != &dummy_item )
		destroy_item_data(id, 1);

	return 0;
}

void itemdb_reload(void)
{
	struct s_mapiterator* iter;
	struct map_session_data* sd;

	int i;

	// clear the previous itemdb data
	for( i = 0; i < ARRAYLENGTH(itemdb_array); ++i )
		if( itemdb_array[i] )
			destroy_item_data(itemdb_array[i], 1);

	itemdb_other->clear(itemdb_other, itemdb_final_sub);

	memset(itemdb_array, 0, sizeof(itemdb_array));

	// read new data
	itemdb_read();

	// readjust itemdb pointer cache for each player
	iter = mapit_geteachpc();
	for( sd = (struct map_session_data*)mapit_first(iter); mapit_exists(iter); sd = (struct map_session_data*)mapit_next(iter) )
	{
		memset(sd->item_delay, 0, sizeof(sd->item_delay));  // reset item delays
		pc_setinventorydata(sd);
	}
	mapit_free(iter);
}

void do_final_itemdb(void)
{
	int i;

	if (itemdb_packages) {
		for (i = 0; i < itemdb_package_count; i++) {
			if (itemdb_packages[i].random_groups) {
				int j;
				for (j = 0; j < itemdb_packages[i].random_qty; j++)
					aFree(itemdb_packages[i].random_groups[j].random_list);
				aFree(itemdb_packages[i].random_groups);
			}
			if (itemdb_packages[i].must_items)
				aFree(itemdb_packages[i].must_items);
		}
		aFree(itemdb_packages);
		itemdb_packages = NULL;
	}
	itemdb_package_count = 0;

	for( i = 0; i < ARRAYLENGTH(itemdb_array); ++i )
		if( itemdb_array[i] )
			destroy_item_data(itemdb_array[i], 1);

	itemdb_other->destroy(itemdb_other, itemdb_final_sub);
	destroy_item_data(&dummy_item, 0);
}

int do_init_itemdb(void)
{
	itemdb_packages = NULL;
	itemdb_package_count = 0;

	memset(itemdb_array, 0, sizeof(itemdb_array));
	itemdb_other = idb_alloc(DB_OPT_BASE); 
	create_dummy_data(); //Dummy data item.
	itemdb_read();

	return 0;
}
