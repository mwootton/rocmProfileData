# Generated by Django 2.2.4 on 2019-08-13 17:09

from django.db import migrations, models


class Migration(migrations.Migration):

    dependencies = [
        ('rocpd', '0001_initial'),
    ]

    operations = [
        migrations.AddIndex(
            model_name='string',
            index=models.Index(fields=['string'], name='rocpd_strin_string_c7b9cd_idx'),
        ),
    ]